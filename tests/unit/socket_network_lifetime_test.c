#define main user_io_lifetime_main
#include "user_io_lifetime_test.c"
#undef main
#undef active
#include "lume/io.h"
#include "lume/socket_file.h"
#include "lume/datagram.h"
#include "lume/socket_buffer.h"
#include "lume/sandbox_policy.h"
#include "lwip/ip.h"
#include "lwip/udp.h"

/* VM, VFS and worker execution are real. The transport callbacks below model
 * ownership and a device wait without running a network stack or a device. */
static struct netif network_interface;
struct netif *netif_list = &network_interface;
struct netif *netif_default = &network_interface;
struct ip_globals ip_data;
static struct udp_pcb network_udp;
static udp_recv_fn network_receive;
static void *network_context;
static struct pbuf network_packets[2];
static unsigned char network_data[8192];
static unsigned network_sends, network_reads, packet_allocations, packet_frees;
static unsigned network_pauses;
static bool packet_live;
static bool oversized_receive;
static bool oversized_send;
static unsigned pause_at;
static volatile uint32_t network_resume;

enum
{
    PAUSE_NONE,
    PAUSE_TCP,
    PAUSE_ALLOCATE,
    PAUSE_UDP
};

static void transport_wait(unsigned point)
{
    if (pause_at != point)
        return;
    pause_at = PAUSE_NONE;
    ++network_pauses;
    CHECK(lume_worker_wait_bits(&network_resume, 1, ticks + 1000000));
}

int lume_sandbox_open(struct lume_files *owner, const char *path, uint64_t flags)
{
    CHECK(owner && owner->identity.sandbox_profile == LUME_SANDBOX_NONE);
    (void)path;
    (void)flags;
    return 0;
}

int lume_sandbox_path(struct lume_files *owner, const char *path)
{
    return lume_sandbox_open(owner, path, 0);
}

bool lume_tty_slave_output(struct lume_files *owner, unsigned fd)
{
    CHECK(owner && owner->identity.sandbox_profile == LUME_SANDBOX_NONE);
    (void)fd;
    return false;
}

bool lume_net_start(void)
{
    return true;
}

struct netif *lume_ip4_route_src(const ip4_addr_t *source, const ip4_addr_t *target)
{
    CHECK(source && target);
    return &network_interface;
}

struct netif *ip4_route_src(const ip4_addr_t *source, const ip4_addr_t *target)
{
    return lume_ip4_route_src(source, target);
}

u8_t ip4_addr_isbroadcast_u32(u32_t address, const struct netif *netif)
{
    CHECK(netif == &network_interface);
    return address == UINT32_MAX;
}

struct udp_pcb *udp_new(void)
{
    memset(&network_udp, 0, sizeof(network_udp));
    return &network_udp;
}

void udp_remove(struct udp_pcb *pcb)
{
    CHECK(pcb == &network_udp);
}

void udp_recv(struct udp_pcb *pcb, udp_recv_fn callback, void *context)
{
    CHECK(pcb == &network_udp);
    network_receive = callback;
    network_context = context;
}

err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *address, u16_t port)
{
    CHECK(pcb == &network_udp && address);
    pcb->local_ip = *address;
    pcb->local_port = port ? port : 9001;
    return ERR_OK;
}

err_t udp_connect(struct udp_pcb *pcb, const ip_addr_t *address, u16_t port)
{
    CHECK(pcb == &network_udp && address && port);
    pcb->remote_ip = *address;
    pcb->remote_port = port;
    return ERR_OK;
}

struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t bytes, pbuf_type type)
{
    CHECK(layer == PBUF_TRANSPORT && type == PBUF_RAM);
    CHECK(!packet_live && bytes <= sizeof(network_data));
    ++packet_allocations;
    packet_live = true;
    unsigned first = bytes < 4096 ? bytes : 4096;
    network_packets[0] = (struct pbuf){.payload = network_data, .len = first, .tot_len = bytes};
    if (first != bytes)
    {
        network_packets[1] = (struct pbuf){
            .payload = network_data + first, .len = bytes - first, .tot_len = bytes - first};
        network_packets[0].next = network_packets + 1;
    }
    transport_wait(PAUSE_ALLOCATE);
    return network_packets;
}

err_t pbuf_take(struct pbuf *packet, const void *data, u16_t bytes)
{
    CHECK(packet == network_packets && packet_live && bytes <= sizeof(network_data));
    memcpy(network_data, data, bytes);
    return ERR_OK;
}

u16_t pbuf_copy_partial(const struct pbuf *packet, void *output, u16_t bytes, u16_t offset)
{
    CHECK(packet == network_packets && packet_live && offset + bytes <= packet->tot_len);
    memcpy(output, network_data + offset, bytes);
    return bytes;
}

u8_t pbuf_free(struct pbuf *packet)
{
    CHECK(packet == network_packets && packet_live);
    packet_live = false;
    ++packet_frees;
    return 1;
}

err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *packet, const ip_addr_t *address, u16_t port)
{
    CHECK(pcb == &network_udp && packet == network_packets && packet_live);
    CHECK(address && port == 9002);
    ++network_sends;
    transport_wait(PAUSE_UDP);
    CHECK(packet_live);
    for (unsigned i = 0; i < packet->tot_len; ++i)
        CHECK(network_data[i] == 0xa7);
    return ERR_OK;
}

int lume_stream_nowait(unsigned owner, unsigned op, struct lume_stream_request *request)
{
    CHECK(owner && request && request->handle == 42);
    if (op == LUME_STREAM_CLOSE)
        return 0;
    CHECK(op == LUME_STREAM_SEND || op == LUME_STREAM_RECEIVE);
    CHECK((uintptr_t)request->data != destination && request->count <= LUME_IO_BYTES);
    if (op == LUME_STREAM_SEND)
    {
        ++network_sends;
        transport_wait(PAUSE_TCP);
        for (unsigned i = 0; i < request->count; ++i)
            CHECK(request->data[i] == 0xa7);
        if (oversized_send)
            return request->count + 1;
    }
    else
    {
        ++network_reads;
        transport_wait(PAUSE_TCP);
        memset(request->data, 0x65, request->count);
        if (oversized_receive)
            return request->count + 1;
    }
    return request->count;
}

int lume_stream_socket_create(unsigned owner, unsigned op, struct lume_stream_request *request,
                              unsigned options)
{
    CHECK(owner && op == LUME_STREAM_CONNECT_ASYNC && request && !options);
    request->handle = 42;
    return 0;
}

int lume_stream_socket_options(unsigned owner, unsigned handle, unsigned options)
{
    (void)owner;
    (void)handle;
    (void)options;
    CHECK(false);
    return -LUME_ENOSYS;
}

int lume_stream_socket_local(unsigned owner, unsigned handle, unsigned char address[4],
                             unsigned *port)
{
    (void)owner;
    (void)handle;
    (void)address;
    (void)port;
    CHECK(false);
    return -LUME_ENOSYS;
}

int lume_stream_socket_error(unsigned owner, unsigned handle, unsigned *error, bool *pending)
{
    (void)owner;
    (void)handle;
    (void)error;
    (void)pending;
    CHECK(false);
    return -LUME_ENOSYS;
}

static int network_fd;
static bool datagram_operation, output_operation;
static unsigned network_operation, message_flags;
static unsigned network_count;
static int64_t network_result;
static struct lume_socket_message returned_message;
static struct lume_socket_request returned_metadata;

static void network_transfer(void *context)
{
    (void)context;
    struct lume_user_buffer_token token = {0};
    struct lume_socket_buffer buffer = {.payload = network_count ? &token : NULL};
    network_result = network_count ? lume_user_buffer_acquire(&token, 1, destination, network_count,
                                                              output_operation)
                                   : 0;
    if (network_result)
        return;
    if (datagram_operation)
    {
        returned_metadata = (struct lume_socket_request){.version = LUME_SOCKET_VERSION,
                                                         .bytes = sizeof(returned_metadata),
                                                         .fd = network_fd,
                                                         .family = 2,
                                                         .buffer = destination,
                                                         .count = network_count};
        if (!output_operation)
        {
            returned_metadata.address[0] = 10;
            returned_metadata.address[3] = 2;
            returned_metadata.port = 9002;
        }
        network_result =
            lume_datagram_call_buffer(&files[0], network_operation, &returned_metadata, &buffer);
    }
    else
    {
        returned_message = (struct lume_socket_message){.version = LUME_SOCKET_MESSAGE_VERSION,
                                                        .bytes = sizeof(returned_message),
                                                        .buffer = destination,
                                                        .count = network_count,
                                                        .flags = message_flags};
        network_result =
            lume_socket_message_call_buffer(&files[0], network_operation, network_fd,
                                            &returned_message, &returned_metadata, &buffer);
    }
    lume_user_buffer_release(&token);
}

static void begin_network(unsigned pause)
{
    network_resume = 0;
    pause_at = pause;
    CHECK(lume_worker_submit_on(LUME_WORK_NETWORK, network_transfer, NULL));
    CHECK(lume_worker_resume_on(LUME_WORK_NETWORK));
    CHECK(lume_worker_busy_on(LUME_WORK_NETWORK) == (pause != PAUSE_NONE));
}

static void end_network(void)
{
    active_root = 2;
    network_resume = 1;
    ticks += 100;
    CHECK(lume_worker_resume_on(LUME_WORK_NETWORK));
    CHECK(!lume_worker_busy_on(LUME_WORK_NETWORK));
}

static int make_socket(unsigned type)
{
    struct lume_socket_request request = {
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(request), .family = 2, .type = type};
    CHECK(lume_socket_call(&files[0], LUME_SOCKET_CREATE, &request) == 0);
    int fd = request.fd;
    request.address[0] = 10;
    request.address[3] = 2;
    request.port = 9002;
    CHECK(lume_socket_call(&files[0], LUME_SOCKET_CONNECT, &request) == 0);
    return fd;
}

static void expect_network_destination(unsigned char value)
{
    CHECK(active_root == 2);
    CHECK(lume_user_buffer(destination, 8192, false));
    for (unsigned offset = 0; offset < 8192; offset += 4096)
    {
        uint64_t physical;
        CHECK(lume_mmu_translate(2, destination + offset, false, true, false, &physical));
        for (unsigned i = 0; i < 4096; ++i)
            CHECK(((unsigned char *)(uintptr_t)physical)[i] == value);
    }
}

static void tcp_cases(void)
{
    network_fd = make_socket(1);
    datagram_operation = false;
    network_count = 4096;
    message_flags = 0;
    shared_setup();
    output_operation = true;
    network_operation = LUME_SOCKET_RECEIVE_MESSAGE;
    network_count = 8192;
    oversized_receive = true;
    begin_network(PAUSE_NONE);
    CHECK(network_result == -LUME_EIO);
    expect_network_destination(0xa7);
    oversized_receive = false;
    shared_cleanup();
    shared_setup();
    output_operation = false;
    network_operation = LUME_SOCKET_SEND_MESSAGE;
    unsigned sends_before = network_sends;
    oversized_send = true;
    begin_network(PAUSE_NONE);
    CHECK(network_result == -LUME_EIO && network_sends == sends_before + 1);
    expect_network_destination(0xa7);
    oversized_send = false;
    shared_cleanup();
    output_operation = true;
    network_operation = LUME_SOCKET_RECEIVE_MESSAGE;
    uint64_t saved_destination = destination;
    unsigned reads_before = network_reads;
    destination = UINT64_MAX;
    network_count = 0;
    begin_network(PAUSE_NONE);
    CHECK(network_result == 0 && network_reads == reads_before);
    destination = saved_destination;
    network_count = 4096;
    for (unsigned sending = 0; sending < 2; ++sending)
    {
        shared_setup();
        output_operation = !sending;
        network_operation = sending ? LUME_SOCKET_SEND_MESSAGE : LUME_SOCKET_RECEIVE_MESSAGE;
        unsigned before = sending ? network_sends : network_reads;
        unsigned live = allocation_live;
        allocation_budget = 0;
        begin_network(PAUSE_TCP);
        CHECK(allocation_live == live);
        allocation_budget = -1;
        active_root = 4;
        CHECK(vm(1, LUME_VM_RESIZE, 0, 0, 0, 0, other_fd) == 0);
        end_network();
        CHECK(network_result == (sending ? 4096 : -LUME_EFAULT));
        CHECK((sending ? network_sends : network_reads) == before + 1);
        shared_cleanup();
    }
    shared_setup();
    output_operation = true;
    network_operation = LUME_SOCKET_RECEIVE_MESSAGE;
    unsigned before = network_reads;
    begin_network(PAUSE_TCP);
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, LUME_PROT_READ, 0, -1) == 0);
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, 3, 0, -1) == 0);
    end_network();
    CHECK(network_result == -LUME_EFAULT && network_reads == before + 1);
    expect_network_destination(0xa7);
    shared_cleanup();
    shared_setup();
    begin_network(PAUSE_TCP);
    CHECK(vm(0, LUME_VM_UNMAP, destination, 8192, 0, 0, -1) == 0);
    CHECK(vm(0, LUME_VM_MAP, destination, 8192, 3, LUME_MAP_SHARED | LUME_MAP_FIXED, shared_fd) ==
          (int64_t)destination);
    CHECK(lume_user_buffer(destination, 8192, true));
    fill_destination(0x3c);
    end_network();
    CHECK(network_result == -LUME_EFAULT);
    expect_network_destination(0x3c);
    shared_cleanup();
    shared_setup();
    message_flags = LUME_SOCKET_MESSAGE_PEEK;
    before = network_reads;
    begin_network(PAUSE_TCP);
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, LUME_PROT_READ, 0, -1) == 0);
    CHECK(vm(0, LUME_VM_PROTECT, destination, 8192, 3, 0, -1) == 0);
    end_network();
    CHECK(network_result == -LUME_EFAULT && network_reads == before + 1);
    begin_network(PAUSE_NONE);
    CHECK(network_result == 4096 && network_reads == before + 1);
    uint64_t physical;
    CHECK(lume_mmu_translate(2, destination, true, true, false, &physical));
    for (unsigned i = 0; i < 4096; ++i)
        CHECK(((unsigned char *)(uintptr_t)physical)[i] == 0x65);
    message_flags = 0;
    begin_network(PAUSE_NONE);
    CHECK(network_result == 4096 && network_reads == before + 1);
    shared_cleanup();
    shared_setup();
    output_operation = false;
    network_operation = LUME_SOCKET_SEND_MESSAGE;
    before = network_sends;
    begin_network(PAUSE_TCP);
    CHECK(lume_vfs_close(&files[0], network_fd) == 0);
    CHECK(lume_socket_reap_pending());
    lume_user_buffer_invalidate_root(0);
    end_network();
    CHECK(network_result == 4096 && network_sends == before + 1);
    shared_cleanup();
    lume_socket_reap();
}

static void queue_datagram(unsigned bytes)
{
    CHECK(network_receive && bytes <= sizeof(network_data));
    struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)bytes, PBUF_RAM);
    memset(network_data, 0x39, bytes);
    ip_addr_t address;
    IP_ADDR4(&address, 10, 0, 0, 2);
    network_receive(network_context, &network_udp, packet, &address, 9002);
    CHECK(!packet_live);
}

static void udp_cases(void)
{
    network_fd = make_socket(2);
    datagram_operation = true;
    output_operation = false;
    network_operation = LUME_SOCKET_SENDTO;
    network_count = 8192;
    for (unsigned point = PAUSE_ALLOCATE; point <= PAUSE_UDP; ++point)
    {
        shared_setup();
        unsigned before = network_sends, freed_before = packet_frees;
        begin_network(point);
        CHECK(packet_live);
        active_root = 4;
        CHECK(vm(1, LUME_VM_RESIZE, 0, 0, 0, 0, other_fd) == 0);
        CHECK(packet_live && packet_frees == freed_before);
        end_network();
        CHECK(network_result == (point == PAUSE_ALLOCATE ? -LUME_EFAULT : 8192));
        CHECK(network_sends == before + (point == PAUSE_UDP));
        CHECK(!packet_live && packet_frees == freed_before + 1);
        shared_cleanup();
    }
    shared_setup();
    queue_datagram(6000);
    output_operation = true;
    network_operation = LUME_SOCKET_RECVFROM;
    network_count = 4096;
    begin_network(PAUSE_NONE);
    CHECK(network_result == 4096 && returned_metadata.count == 6000);
    CHECK(returned_metadata.flags & LUME_SOCKET_DATAGRAM_TRUNCATED);
    uint64_t physical;
    CHECK(lume_mmu_translate(2, destination, true, true, false, &physical));
    for (unsigned i = 0; i < 4096; ++i)
        CHECK(((unsigned char *)(uintptr_t)physical)[i] == 0x39);
    CHECK(lume_mmu_translate(2, destination + 4096, true, true, false, &physical));
    for (unsigned i = 0; i < 4096; ++i)
        CHECK(((unsigned char *)(uintptr_t)physical)[i] == 0xa7);
    begin_network(PAUSE_NONE);
    CHECK(network_result == -LUME_EAGAIN);
    shared_cleanup();
    CHECK(lume_vfs_close(&files[0], network_fd) == 0);
    lume_datagram_reap();
}

#ifndef LUME_SOCKET_NETWORK_LIFETIME_INCLUDE
int main(void)
{
#ifdef _WIN32
    CHECK(ConvertThreadToFiber(NULL));
#endif
    static const struct lume_storage_ops storage_ops = {
        .walk = walk, .read = read_file, .write = write_file};
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(&storage_ops));
    for (unsigned i = 0; i < 3; ++i)
        lume_files_init(&files[i]);
    baseline_frames = frame_live;
    lume_worker_start();
    network_interface.flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    network_interface.mtu = 9000;
    IP_ADDR4(&network_interface.ip_addr, 10, 0, 0, 1);
    tcp_cases();
    udp_cases();
    CHECK(packet_allocations == packet_frees && !packet_live);
    CHECK(!unsafe_borrows && frame_live == baseline_frames);
    for (unsigned i = 0; i < 3; ++i)
        lume_files_release(&files[i]);
    while (retired)
        reap(NULL);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        release_fiber(&workers[i].fiber);
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
        free(extra_stacks[i]);
    printf("PASS_SOCKET_NETWORK_LIFETIME checks=%u sends=%u reads=%u pauses=%u pbufs=%u\n", checks,
           network_sends, network_reads, network_pauses, packet_allocations);
    return 0;
}
#endif
