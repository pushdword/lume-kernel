/* The W5500 provides hardware TCP/IP through the PMOD1 SPI controller. Callers yield and retry
 * EAGAIN or EINPROGRESS instead of waiting inside the driver. Raw FIFO access requires the
 * capability bit and magic value. Legacy controllers use byte I/O. The driver leaves NIC reset
 * and RTC control registers unchanged. Register behavior follows the WIZnet W5500 datasheet
 * v1.1.0 and TCP application note. */
#include "network.h"
#include "clock_io.h"
#include <string.h>

#define BASE ((uintptr_t)0xf0007000u)
#define SOCKETS 8u
#define BUF_SIZE 2048u
#define COMMAND_POLLS 16u
#define BYTE_POLLS 1024u
#define RAW_POLLS 16384u
#define RAW_CAPS 0xe0000000u
#define RAW_MAGIC 0x31524253u
#ifdef _MSC_VER
typedef uint32_t net_alias_word;
#else
typedef uint32_t net_alias_word __attribute__((__may_alias__));
#endif
#define CONNECT_MS 15000u
#define SEND_MS 10000u
extern uint32_t uptime_ms;
#ifdef LUME_NET_TEST_MMIO
extern uint32_t lume_net_mmio_read(uintptr_t address);
extern void lume_net_mmio_write(uintptr_t address, uint32_t value);
#else
static uint32_t lume_net_mmio_read(uintptr_t p)
{
    return *(volatile uint32_t *)p;
}

static void lume_net_mmio_write(uintptr_t p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
}
#endif

enum
{
    CREATED,
    UDP_OPENING,
    UDP_READY,
    LISTEN_OPENING,
    LISTEN_READY,
    CONNECT_OPENING,
    CONNECTING,
    CONNECTED,
    PEER_CLOSED,
    FAILED
};

struct socket_state
{
    unsigned used, owner, protocol, hardware, phase, port;
    unsigned pending, tx_start, tx_length, send_command;
    uint32_t started, sent;
    unsigned closing, disconnect_sent;
    uint32_t close_started;
    int error;
    unsigned char peer[4];
    unsigned peer_port;
};
static struct socket_state sockets[SOCKETS];
static struct lume_net_config configuration = {LUME_NET_VERSION,
                                               sizeof(struct lume_net_config),
                                               {2, 0x58, 0x32, 0x25, 3, 0x17},
                                               {0, 0},
                                               {0, 0, 0, 0},
                                               {255, 255, 255, 0},
                                               {0, 0, 0, 0},
                                               {0, 0, 0, 0},
                                               0};
static unsigned ready, spi_hz, raw_blocks;

static int zero_ip(const unsigned char p[4])
{
    return !(p[0] | p[1] | p[2] | p[3]);
}

static int local_peer(const unsigned char p[4])
{
    return p[0] == 172 && p[1] == 30 && p[2] == 1 && p[3] && p[3] != 255;
}

static int broadcast(const unsigned char p[4])
{
    return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255;
}

static unsigned block(const struct socket_state *s)
{
    return 1u + 4u * s->hardware;
}

static inline int byte_done(void)
{
    unsigned budget = BYTE_POLLS;
    while (lume_net_mmio_read(BASE + 12) & 1u)
    {
        if (!--budget)
            return 0;
        if (!(budget & 127u))
            io_clock_checkpoint();
    }
    return 1;
}

static int header_byte(unsigned byte)
{
    lume_net_mmio_write(BASE + 8, byte);
    return byte_done();
}

/* The raw engine consumes exactly the payload bytes. The PMOD instance disables SD CRC framing.
 * Keep CS asserted across the W5500 header and every payload block. */
static int raw_payload(unsigned char *out, const unsigned char *in, unsigned n)
{
    while (n)
    {
        unsigned take = n > 512u ? 512u : n;
        uint32_t status = lume_net_mmio_read(BASE + 12);
        if ((status & 1u) || ((status >> 8) & 255u))
            return 0;
        if (in)
        {
            for (unsigned i = 0; i < take; i += 4u)
            {
                unsigned bytes = take - i;
                if (bytes > 4u)
                    bytes = 4u;
                uint32_t word = 0;
                if (bytes == 4u && !((uintptr_t)(in + i) & 3u))
                    word = *(const net_alias_word *)(const void *)(in + i);
                else
                    for (unsigned j = 0; j < bytes; j++)
                        word |= (uint32_t)in[i + j] << (8u * j);
                lume_net_mmio_write(BASE + 44, word);
            }
            lume_net_mmio_write(BASE + 40, 0x40000000u | take);
        }
        else
        {
            lume_net_mmio_write(BASE + 40, 0x80000000u | take);
            unsigned received = 0, budget = RAW_POLLS;
            while (received < take && budget--)
            {
                status = lume_net_mmio_read(BASE + 12);
                unsigned words = (status >> 8) & 255u;
                if (words > (take - received + 3u) / 4u)
                    return 0;
                if (!words && (status & 8u))
                    return 0;
                while (words--)
                {
                    uint32_t word = lume_net_mmio_read(BASE + 44);
                    unsigned bytes = take - received;
                    if (bytes > 4u)
                        bytes = 4u;
                    if (out && bytes == 4u && !((uintptr_t)(out + received) & 3u))
                    {
                        *(net_alias_word *)(void *)(out + received) = word;
                        received += 4u;
                    }
                    else
                        for (unsigned j = 0; j < bytes; j++)
                        {
                            if (out)
                                out[received] = (unsigned char)(word >> (8u * j));
                            received++;
                        }
                }
                if (!(budget & 127u))
                    io_clock_checkpoint();
            }
            if (received != take)
                return 0;
        }
        unsigned done = 0;
        for (unsigned budget = RAW_POLLS; budget; budget--)
        {
            status = lume_net_mmio_read(BASE + 12);
            if (status & 8u)
            {
                if ((status & 1u) || ((status >> 8) & 255u))
                    return 0;
                done = 1;
                break;
            }
            if (!(budget & 127u))
                io_clock_checkpoint();
        }
        if (!done)
            return 0;
        if (in)
            in += take;
        if (out)
            out += take;
        n -= take;
        io_clock_checkpoint();
    }
    return 1;
}

static int access_bytes(unsigned b, unsigned address, unsigned char *out, const unsigned char *in,
                        unsigned n)
{
    if (!n || n > LUME_NET_IO_MAX || b > 31 || address > 65535)
        return 0;
    lume_net_mmio_write(BASE + 4, 0);
    int ok = header_byte(address >> 8) && header_byte(address & 255) &&
             header_byte((b << 3) | (in ? 4u : 0));
    if (ok && raw_blocks && n >= 16u)
        ok = raw_payload(out, in, n);
    else if (ok && in)
    {
        lume_net_mmio_write(BASE + 8, in[0]);
        for (unsigned i = 1; ok && i < n; i++)
        {
            unsigned next = in[i];
            ok = byte_done();
            if (ok)
                lume_net_mmio_write(BASE + 8, next);
            if ((i & 127u) == 127u)
                io_clock_checkpoint();
        }
        if (ok)
            ok = byte_done();
    }
    else if (ok)
    {
        lume_net_mmio_write(BASE + 8, 255);
        for (unsigned i = 0; ok && i < n; i++)
        {
            ok = byte_done();
            if (!ok)
                break;
            unsigned value = lume_net_mmio_read(BASE + 8) & 255u;
            /* Overlap the next SPI byte with storing/checkpointing this one.
             * No other caller can touch PMOD while the network owner is held. */
            if (i + 1u < n)
                lume_net_mmio_write(BASE + 8, 255);
            if (out)
                out[i] = (unsigned char)value;
            if ((i & 127u) == 127u)
                io_clock_checkpoint();
        }
    }
    if (ok)
        lume_net_mmio_write(BASE + 4, 1);
    else
        lume_net_mmio_write(BASE + 20, 1);
    return ok;
}

static int get(unsigned b, unsigned a)
{
    unsigned char v;
    return access_bytes(b, a, &v, 0, 1) ? v : -1;
}

static int put(unsigned b, unsigned a, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return access_bytes(b, a, 0, &c, 1);
}

static int get_word(unsigned b, unsigned a)
{
    unsigned char v[2];
    return access_bytes(b, a, v, 0, 2) ? (int)((unsigned)v[0] * 256u + v[1]) : -1;
}

static int put_word(unsigned b, unsigned a, unsigned v)
{
    unsigned char bytes[2] = {(unsigned char)(v >> 8), (unsigned char)v};
    return access_bytes(b, a, 0, bytes, 2);
}

static int stable_word(unsigned b, unsigned a, unsigned *value)
{
    int old = get_word(b, a);
    if (old < 0)
        return -LUME_NET_EIO;
    for (unsigned i = 0; i < 8; ++i)
    {
        int next = get_word(b, a);
        if (next < 0)
            return -LUME_NET_EIO;
        if (old == next)
        {
            *value = (unsigned)next;
            return 0;
        }
        old = next;
    }
    return -LUME_NET_EAGAIN;
}

static int command(unsigned b, unsigned value)
{
    if (!put(b, 1, value))
        return 0;
    /* CR clears when accepted, not when CONNECT/SEND completes. Status and IR
     * are handled in service(), on later syscalls, without spinning for peers. */
    for (unsigned i = 0; i < COMMAND_POLLS; ++i)
    {
        int v = get(b, 1);
        if (v < 0)
            return 0;
        if (!v)
            return 1;
    }
    return 0;
}

static int write_configuration(const struct lume_net_config *c)
{
    if (!access_bytes(0, 9, 0, c->mac, 6) || !access_bytes(0, 15, 0, c->address, 4) ||
        !access_bytes(0, 5, 0, c->netmask, 4) || !access_bytes(0, 1, 0, c->gateway, 4))
        return 0;
    unsigned char actual[6];
    const unsigned char *expected[] = {c->mac, c->address, c->netmask, c->gateway};
    const unsigned addresses[] = {9, 15, 5, 1}, sizes[] = {6, 4, 4, 4};
    for (unsigned i = 0; i < 4; ++i)
        if (!access_bytes(0, addresses[i], actual, 0, sizes[i]) ||
            memcmp(actual, expected[i], sizes[i]))
            return 0;
    return 1;
}

static int ensure(void)
{
    if (ready)
        return 0;
    if (lume_net_mmio_read(BASE + 32) != 0x31444d50u || !(lume_net_mmio_read(BASE + 24) & 1u))
        return -LUME_NET_ENODEV;
    unsigned hz = lume_net_mmio_read(BASE + 16);
    if (hz < 1000000 || hz > 100000000)
        return -LUME_NET_ENODEV;
    unsigned div = (hz + 29999999u) /
                   30000000u; /* A 30 MHz source with divider 1 limits the SPI clock to 15 MHz. */
    lume_net_mmio_write(BASE, div);
    spi_hz = hz / (2u * div);
    lume_net_mmio_write(BASE + 20, 1);
    raw_blocks = 0;
    if ((lume_net_mmio_read(BASE + 12) & RAW_CAPS) == RAW_CAPS)
    {
        if (lume_net_mmio_read(BASE + 40) != RAW_MAGIC)
            return -LUME_NET_ENODEV;
        raw_blocks = 1;
    }
    if (get(0, 0x39) != 4 || get(0, 0x39) != 4)
        return -LUME_NET_ENODEV;
    /* Fresh OS ownership. Default socket memory is made explicit, and ping is
     * enabled. Do not reuse BIOS udp_close(), which clears common IP registers. */
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (!command(1 + 4 * i, 0x10) || !put(1 + 4 * i, 0x1e, 2) || !put(1 + 4 * i, 0x1f, 2))
            return -LUME_NET_EIO;
    if (!put(0, 0, 0) || !write_configuration(&configuration))
        return -LUME_NET_EIO;
    ready = 1;
    return 0;
}

static int handle(unsigned owner, int h)
{
    if (h < 1 || h > (int)SOCKETS || !sockets[h - 1].used || sockets[h - 1].owner != owner)
        return -1;
    return h - 1;
}

static int free_slot(void)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (!sockets[i].used)
            return (int)i;
    return -1;
}

static unsigned free_hardware(void)
{
    for (unsigned h = 0; h < SOCKETS; ++h)
    {
        unsigned found = 0;
        for (unsigned i = 0; i < SOCKETS; ++i)
            if (sockets[i].used && sockets[i].hardware == h)
                found = 1;
        if (!found)
            return h;
    }
    return SOCKETS;
}

static unsigned used(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SOCKETS; ++i)
        n += sockets[i].used != 0;
    return n;
}

static int port_used(const struct socket_state *s, unsigned port)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (&sockets[i] != s && sockets[i].used && sockets[i].protocol == s->protocol &&
            sockets[i].port == port)
            return 1;
    return 0;
}

static unsigned ephemeral(const struct socket_state *s)
{
    for (unsigned p = 49152; p < 49216; ++p)
        if (!port_used(s, p))
            return p;
    return 0;
}

static int open_hardware(struct socket_state *s)
{
    unsigned b = block(s);
    /* Sn_MR.ND bit 5 requests immediate TCP acknowledgements. UDP uses mode 2. The socket mode
     * register is described on page 44 of the WIZnet W5500 datasheet v1.0.9. */
    return command(b, 0x10) && put(b, 0, s->protocol == LUME_NET_TCP ? 0x21 : 2) &&
           put_word(b, 4, s->port) && put(b, 2, 255) && command(b, 1);
}

static int begin_listen(struct socket_state *s)
{
    if (!open_hardware(s))
        return -LUME_NET_EIO;
    s->phase = LISTEN_OPENING;
    s->started = uptime_ms;
    return 0;
}

static int fail(struct socket_state *s, int error)
{
    s->error = error;
    s->phase = FAILED;
    s->pending = 0;
    return -error;
}

static int service(struct socket_state *s)
{
    if (s->phase == FAILED)
        return -s->error;
    if (s->phase == CREATED)
        return 0;
    unsigned b = block(s);
    unsigned char state[2];
    if (!access_bytes(b, 2, state, 0, 2))
        return -LUME_NET_EIO;
    int ir = state[0], status = state[1];
    if (ir & 8)
    {
        if (!put(b, 2, 8))
            return -LUME_NET_EIO;
        return fail(s, LUME_NET_ETIMEDOUT);
    }
    if (s->pending)
    {
        if (ir & 16)
        {
            unsigned rd;
            int e = stable_word(b, 0x22, &rd);
            if (e)
                return e;
            unsigned moved = (rd - s->tx_start) & 65535u;
            if (moved > s->tx_length)
                return fail(s, LUME_NET_EIO);
            if (!put(b, 2, 16))
                return -LUME_NET_EIO;
            if (moved == s->tx_length)
                s->pending = 0;
            else if (!command(b, s->send_command))
                return -LUME_NET_EIO;
        }
        if (s->pending && (uint32_t)(uptime_ms - s->sent) >= SEND_MS)
            return fail(s, LUME_NET_ETIMEDOUT);
    }
    if (s->phase == UDP_OPENING && status == 0x22)
        s->phase = UDP_READY;
    else if (s->phase == LISTEN_OPENING && status == 0x13)
    {
        if (!command(b, 2))
            return -LUME_NET_EIO;
        s->phase = LISTEN_READY;
    }
    else if (s->phase == CONNECT_OPENING && status == 0x13)
    {
        if (!access_bytes(b, 12, 0, s->peer, 4) || !put_word(b, 16, s->peer_port) || !command(b, 4))
            return -LUME_NET_EIO;
        s->phase = CONNECTING;
    }
    else if (s->phase == CONNECTING && (status == 0x17 || status == 0x1c))
        s->phase = CONNECTED;
    if ((s->phase == UDP_OPENING || s->phase == LISTEN_OPENING || s->phase == CONNECT_OPENING ||
         s->phase == CONNECTING) &&
        (uint32_t)(uptime_ms - s->started) >= CONNECT_MS)
        return fail(s, LUME_NET_ETIMEDOUT);
    if (s->phase == CONNECTED && status == 0)
        s->phase = PEER_CLOSED;
    return status;
}

static int send_data(struct socket_state *s, unsigned char *data, unsigned length)
{
    if (s->pending)
        return -LUME_NET_EAGAIN;
    if (!length)
        return 0;
    unsigned available;
    int e = stable_word(block(s), 0x20, &available);
    if (e)
        return e;
    if (available > BUF_SIZE)
        return -LUME_NET_EIO;
    if (!available || (s->protocol == LUME_NET_UDP && available < length))
        return -LUME_NET_EAGAIN;
    if (length > available)
        length = available;
    unsigned b = block(s);
    int pointer = get_word(b, 0x24), rd = get_word(b, 0x22);
    if (pointer < 0 || rd < 0)
        return -LUME_NET_EIO;
    if (!put(b, 2, 0x18) || !access_bytes(b + 1, (unsigned)pointer, 0, data, length) ||
        !put_word(b, 0x24, ((unsigned)pointer + length) & 65535u) ||
        !command(b, s->send_command ? s->send_command : 0x20))
        return -LUME_NET_EIO;
    if (!s->send_command)
        s->send_command = 0x20;
    s->pending = 1;
    s->sent = uptime_ms;
    s->tx_start = (unsigned)rd;
    s->tx_length = length;
    return (int)length;
}

static int receive_data(struct socket_state *s, struct lume_net_request *r, unsigned char *data)
{
    unsigned available, b = block(s);
    int e = stable_word(b, 0x26, &available);
    if (e)
        return e;
    if (available > BUF_SIZE)
        return -LUME_NET_EIO;
    int pointer = get_word(b, 0x28);
    if (pointer < 0)
        return -LUME_NET_EIO;
    unsigned count = r->length, advance;
    if (s->protocol == LUME_NET_UDP)
    {
        if (available < 8)
            return -LUME_NET_EAGAIN;
        unsigned char h[8];
        if (!access_bytes(b + 2, (unsigned)pointer, h, 0, 8))
            return -LUME_NET_EIO;
        unsigned bytes = (unsigned)h[6] * 256u + h[7];
        if (bytes > BUF_SIZE - 8)
        {
            /* A corrupt/impossible record cannot safely preserve its boundary.
             * Reopening flushes it instead of interpreting payload as headers. */
            if (!open_hardware(s))
                return -LUME_NET_EIO;
            s->phase = UDP_OPENING;
            return -LUME_NET_EMSGSIZE;
        }
        if (available < bytes + 8)
            return -LUME_NET_EAGAIN;
        advance = bytes + 8;
        unsigned peer_port = (unsigned)h[4] * 256u + h[5];
        int permitted = local_peer(h);
        int error = !permitted ? LUME_NET_EACCES : bytes > count ? LUME_NET_EMSGSIZE : 0;
        if (!error && bytes &&
            !access_bytes(b + 2, ((unsigned)pointer + 8u) & 65535u, data, 0, bytes))
            return -LUME_NET_EIO;
        if (!put_word(b, 0x28, ((unsigned)pointer + advance) & 65535u) || !command(b, 0x40))
            return -LUME_NET_EIO;
        if (error)
            return -error;
        memcpy(r->address, h, 4);
        r->port = peer_port;
        return (int)bytes;
    }
    if (!count)
        return 0;
    if (!available)
    {
        int status = get(b, 3);
        return status == 0 || status == 0x1c ? 0 : -LUME_NET_EAGAIN;
    }
    if (count > available)
        count = available;
    if (!access_bytes(b + 2, (unsigned)pointer, data, 0, count) ||
        !put_word(b, 0x28, ((unsigned)pointer + count) & 65535u) || !command(b, 0x40))
        return -LUME_NET_EIO;
    return (int)count;
}

static int accept_connection(struct socket_state *listener, struct lume_net_request *r, int status)
{
    if (status != 0x17 && status != 0x1c)
        return -LUME_NET_EAGAIN;
    unsigned char peer[4];
    unsigned b = block(listener);
    int port = get_word(b, 16);
    if (port < 0 || !access_bytes(b, 12, peer, 0, 4))
        return -LUME_NET_EIO;
    if (!local_peer(peer))
    {
        int e = begin_listen(listener);
        return e ? e : -LUME_NET_EACCES;
    }
    int next = free_slot();
    unsigned hardware = free_hardware();
    if (next < 0 || hardware == SOCKETS)
        return -LUME_NET_EMFILE;
    struct socket_state replacement = *listener;
    replacement.hardware = hardware;
    replacement.pending = 0;
    int error = begin_listen(&replacement);
    if (error)
        return error;
    sockets[next] = *listener;
    sockets[next].phase = CONNECTED;
    sockets[next].pending = 0;
    memcpy(sockets[next].peer, peer, 4);
    sockets[next].peer_port = (unsigned)port;
    *listener = replacement;
    memcpy(r->address, peer, 4);
    r->port = (unsigned)port;
    return next + 1;
}

int32_t lume_net_call(unsigned owner, unsigned op, struct lume_net_request *r, unsigned char *data)
{
    if (owner >= 8 || !r || !data || r->version != LUME_NET_VERSION || r->bytes != sizeof *r ||
        r->flags)
        return -LUME_NET_EINVAL;
    if (op < LUME_NET_INFO || op > LUME_NET_POLL)
        return -LUME_NET_ENOSYS;
    int error = ensure();
    if (op == LUME_NET_INFO)
    {
        struct lume_net_info info;
        memset(&info, 0, sizeof info);
        info.version = LUME_NET_VERSION;
        info.bytes = sizeof info;
        info.socket_count = SOCKETS;
        info.sockets_used = used();
        info.spi_hz = spi_hz;
        info.configuration = configuration;
        if (!error)
        {
            int phy = get(0, 0x2e);
            if (phy >= 0)
            {
                info.flags = LUME_NET_READY | (phy & 1 ? LUME_NET_LINK : 0) |
                             (!zero_ip(configuration.address) ? LUME_NET_CONFIGURED : 0);
            }
        }
        if (r->length != sizeof info)
            return -LUME_NET_EINVAL;
        memcpy(data, &info, sizeof info);
        return 0;
    }
    if (error)
        return error;
    if (op == LUME_NET_CONFIG)
    {
        if (r->length != sizeof(struct lume_net_config))
            return -LUME_NET_EINVAL;
        struct lume_net_config c;
        memcpy(&c, data, sizeof c);
        if (c.version != LUME_NET_VERSION || c.bytes != sizeof c || c.flags || c.reserved[0] ||
            c.reserved[1] || (c.mac[0] & 1) ||
            !(c.mac[0] | c.mac[1] | c.mac[2] | c.mac[3] | c.mac[4] | c.mac[5]))
            return -LUME_NET_EINVAL;
        int mask_ok =
            (c.netmask[0] == 255 && c.netmask[1] == 255 && c.netmask[2] == 255 && !c.netmask[3]) ||
            (zero_ip(c.address) && zero_ip(c.netmask));
        if ((!zero_ip(c.address) && !local_peer(c.address)) || !mask_ok ||
            (!zero_ip(c.gateway) && !local_peer(c.gateway)))
            return -LUME_NET_EACCES;
        if (zero_ip(c.address))
        {
            /* Root-only administrative lease withdrawal (enforced by syscall
             * marshalling). Stop using the old IP before removing all owners'
             * handles. T1 fresh acquisition intentionally disconnects clients.
             * Never roll back to the expired address if later I/O fails. */
            int ok = access_bytes(0, 15, 0, c.address, 4);
            for (unsigned i = 0; i < SOCKETS; ++i)
                if (sockets[i].used)
                {
                    if (!command(block(&sockets[i]), 0x10))
                        ok = 0;
                    memset(&sockets[i], 0, sizeof sockets[i]);
                }
            if (!write_configuration(&c))
                ok = 0;
            configuration = c;
            if (!ok)
            {
                ready = 0;
                return -LUME_NET_EIO;
            }
            return 0;
        }
        int same = !memcmp(c.mac, configuration.mac, 6) &&
                   !memcmp(c.address, configuration.address, 4) &&
                   !memcmp(c.netmask, configuration.netmask, 4) &&
                   !memcmp(c.gateway, configuration.gateway, 4);
        for (unsigned i = 0; i < SOCKETS; ++i)
            if (sockets[i].used && sockets[i].protocol == LUME_NET_TCP)
            {
                if (!same)
                    return -LUME_NET_EBUSY;
                configuration = c;
                return 0; /* Lease renewal/DNS metadata, no HW rebind. */
            }
        if (!write_configuration(&c))
        {
            if (!write_configuration(&configuration))
            {
                ready = 0;
                for (unsigned i = 0; i < SOCKETS; ++i)
                    if (sockets[i].used)
                        (void)fail(&sockets[i], LUME_NET_EIO);
            }
            return -LUME_NET_EIO;
        }
        configuration = c;
        return 0;
    }
    if (op == LUME_NET_SOCKET)
    {
        if (r->protocol != LUME_NET_TCP && r->protocol != LUME_NET_UDP)
            return -LUME_NET_EOPNOTSUPP;
        int n = free_slot();
        unsigned hardware = free_hardware();
        if (n < 0 || hardware == SOCKETS)
            return -LUME_NET_EMFILE;
        memset(&sockets[n], 0, sizeof sockets[n]);
        sockets[n].used = 1;
        sockets[n].owner = owner;
        sockets[n].hardware = hardware;
        sockets[n].protocol = r->protocol;
        return n + 1;
    }
    int n = handle(owner, r->handle);
    if (n < 0)
        return -LUME_NET_EBADF;
    struct socket_state *s = &sockets[n];
    if (op == LUME_NET_CLOSE)
    {
        /* Let queued TX finish and request TCP FIN. After five seconds from the
         * first CLOSE, force cleanup even if SEND_OK/the peer never arrives. */
        if (!s->closing)
        {
            s->closing = 1;
            s->close_started = uptime_ms;
        }
        if ((uint32_t)(uptime_ms - s->close_started) < 5000u)
        {
            int status = service(s);
            if (status >= 0 && s->pending)
                return -LUME_NET_EAGAIN;
            if (status >= 0 && s->protocol == LUME_NET_TCP &&
                (status == 0x17 || status == 0x1c || (s->disconnect_sent && status != 0)))
            {
                if (!s->disconnect_sent)
                {
                    if (!command(block(s), 8))
                        return -LUME_NET_EIO;
                    s->disconnect_sent = 1;
                }
                return -LUME_NET_EAGAIN;
            }
        }
        int ok = command(block(s), 0x10);
        memset(s, 0, sizeof *s);
        return ok ? 0 : -LUME_NET_EIO;
    }
    int status = service(s);
    if (op == LUME_NET_POLL)
    {
        unsigned events = 0;
        if (status < 0)
            events = LUME_NET_POLLERR | LUME_NET_POLLHUP;
        else if (s->phase == PEER_CLOSED)
            events = LUME_NET_POLLHUP | LUME_NET_POLLIN;
        else if (s->phase == LISTEN_READY)
        {
            if (status == 0x14 || status == 0x17 || status == 0x1c)
                events |= LUME_NET_POLLLISTEN;
            if (status == 0x17 || status == 0x1c)
                events |= LUME_NET_POLLIN;
        }
        else if (s->phase == CONNECTED || s->phase == UDP_READY)
        {
            unsigned value;
            if (r->events & LUME_NET_POLLIN)
            {
                int e = stable_word(block(s), 0x26, &value);
                if (e)
                    events |= LUME_NET_POLLERR;
                else if (value >= (s->protocol == LUME_NET_UDP ? 8u : 1u) || status == 0x1c)
                    events |= LUME_NET_POLLIN;
            }
            if ((r->events & LUME_NET_POLLOUT) && !s->pending)
            {
                int e = stable_word(block(s), 0x20, &value);
                if (e)
                    events |= LUME_NET_POLLERR;
                else if (value)
                    events |= LUME_NET_POLLOUT;
            }
        }
        events &= r->events | LUME_NET_POLLERR | LUME_NET_POLLHUP;
        r->events = events;
        return events != 0;
    }
    if (status < 0)
        return status;
    if (op == LUME_NET_BIND)
    {
        if (s->phase != CREATED || !r->port || r->port > 65535 ||
            (!zero_ip(r->address) && memcmp(r->address, configuration.address, 4)))
            return -LUME_NET_EINVAL;
        if (port_used(s, r->port))
            return -LUME_NET_EADDRINUSE;
        s->port = r->port;
        if (s->protocol == LUME_NET_UDP)
        {
            if (!open_hardware(s))
                return -LUME_NET_EIO;
            s->phase = UDP_OPENING;
            s->started = uptime_ms;
        }
        return 0;
    }
    if (op == LUME_NET_LISTEN)
    {
        if (s->protocol != LUME_NET_TCP)
            return -LUME_NET_EOPNOTSUPP;
        if (s->port != 8022 && s->port != 22)
            return -LUME_NET_EACCES;
        if (s->phase != CREATED)
            return -LUME_NET_EINVAL;
        return begin_listen(s);
    }
    if (op == LUME_NET_ACCEPT)
    {
        if (s->phase != LISTEN_READY && s->phase != LISTEN_OPENING)
            return -LUME_NET_EINVAL;
        return accept_connection(s, r, status);
    }
    if (op == LUME_NET_CONNECT)
    {
        if (s->protocol != LUME_NET_TCP)
            return -LUME_NET_EOPNOTSUPP;
        if (!local_peer(r->address))
            return -LUME_NET_EACCES;
        if (zero_ip(configuration.address))
            return -LUME_NET_ENETUNREACH;
        if (!r->port || r->port > 65535)
            return -LUME_NET_EINVAL;
        if (s->phase == CONNECTING || s->phase == CONNECT_OPENING)
            return -LUME_NET_EALREADY;
        if (s->phase == CONNECTED)
            return 0;
        if (s->phase != CREATED)
            return -LUME_NET_EINVAL;
        if (!s->port)
            s->port = ephemeral(s);
        memcpy(s->peer, r->address, 4);
        s->peer_port = r->port;
        if (!open_hardware(s))
            return -LUME_NET_EIO;
        s->phase = CONNECT_OPENING;
        s->started = uptime_ms;
        return -LUME_NET_EINPROGRESS;
    }
    if (op == LUME_NET_SEND || op == LUME_NET_SENDTO)
    {
        if (s->closing)
            return -LUME_NET_EPIPE;
        if (r->length > LUME_NET_IO_MAX)
            return -LUME_NET_EMSGSIZE;
        if (s->protocol == LUME_NET_TCP)
        {
            if (op == LUME_NET_SENDTO)
                return -LUME_NET_EOPNOTSUPP;
            if (s->phase == PEER_CLOSED)
                return -LUME_NET_EPIPE;
            if (s->phase != CONNECTED)
                return -LUME_NET_ENOTCONN;
        }
        else
        {
            if (op != LUME_NET_SENDTO)
                return -LUME_NET_EOPNOTSUPP;
            int dhcp = s->port == 68 && r->port == 67 && broadcast(r->address);
            if (!r->port || r->port > 65535)
                return -LUME_NET_EINVAL;
            if (!dhcp && !local_peer(r->address))
                return -LUME_NET_EACCES;
            if (!dhcp && zero_ip(configuration.address))
                return -LUME_NET_ENETUNREACH;
            if (s->phase != UDP_READY)
                return -LUME_NET_EAGAIN;
            if (s->pending)
                return -LUME_NET_EAGAIN;
            if (!access_bytes(block(s), 12, 0, r->address, 4) || !put_word(block(s), 16, r->port))
                return -LUME_NET_EIO;
            s->send_command = dhcp ? 0x21u : 0x20u;
            if (dhcp)
            {
                const unsigned char mac[6] = {255, 255, 255, 255, 255, 255};
                if (!access_bytes(block(s), 6, 0, mac, 6))
                    return -LUME_NET_EIO;
            }
        }
        return send_data(s, data, r->length);
    }
    if (op == LUME_NET_RECV || op == LUME_NET_RECVFROM)
    {
        if (r->length > LUME_NET_IO_MAX)
            return -LUME_NET_EMSGSIZE;
        if (s->protocol == LUME_NET_TCP && s->phase != CONNECTED && s->phase != PEER_CLOSED)
            return -LUME_NET_ENOTCONN;
        if (s->protocol == LUME_NET_UDP && s->phase != UDP_READY)
            return -LUME_NET_EAGAIN;
        if (s->protocol == LUME_NET_TCP && op == LUME_NET_RECVFROM)
        {
            memcpy(r->address, s->peer, 4);
            r->port = s->peer_port;
        }
        return receive_data(s, r, data);
    }
    return -LUME_NET_EOPNOTSUPP;
}

void lume_net_process_exit(unsigned owner)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (sockets[i].used && sockets[i].owner == owner)
        {
            if (ready)
                (void)command(block(&sockets[i]), 0x10);
            memset(&sockets[i], 0, sizeof sockets[i]);
        }
}
