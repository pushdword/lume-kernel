#include "../../../src/compat/legacy32/x32/uart_queue.h"
#include <stddef.h>
#include <string.h>
#define BASE 0xf0000000u
#ifdef LUME_UART_HOST
extern uint32_t uart_hal_read32(uintptr_t);
extern void uart_hal_write32(uintptr_t, uint32_t);
extern void uart_hal_write8(uintptr_t, unsigned char);
extern uint32_t uart_hal_counter(void);
extern unsigned uart_hal_core(void);

static uint32_t publish_read(volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void publish_write(volatile uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
#else
extern uint32_t lume_perf_counter(void);
extern unsigned core_id(void);

static uint32_t uart_hal_read32(uintptr_t p)
{
    return *(volatile uint32_t *)p;
}

static void uart_hal_write32(uintptr_t p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
}

static void uart_hal_write8(uintptr_t p, unsigned char v)
{
    *(volatile unsigned char *)p = v;
}

static uint32_t uart_hal_counter(void)
{
    return lume_perf_counter();
}

static unsigned uart_hal_core(void)
{
    return core_id();
}

/* X32 has no data cache/store buffer: a natural aligned32-bit RAM access
 * completes on the shared bus before the next instruction. Compiler barriers
 * plus this architectural ordering implement publication, not volatile alone. */
static uint32_t publish_read(volatile uint32_t *p)
{
    uint32_t v = *p;
    __asm__ volatile("" ::: "memory");
    return v;
}

static void publish_write(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile("" ::: "memory");
    *p = v;
}
#endif
extern uint32_t x32_cas(volatile uint32_t *, uint32_t, uint32_t);

enum
{
    FREE = 0,
    WRITING = 1,
    READY = 2
};

struct request
{
    volatile uint32_t state;
    unsigned size, position, cr;
    unsigned char data[UART_REQUEST_BYTES];
};
static struct request requests[UART_REQUESTS];
static volatile uint32_t uart_lock, emergency;
static unsigned active, put_at, take_at, rx_put, rx_take, rx_count, paused_rx;
static volatile uint32_t queued;
static unsigned char rx_bytes[1024];
static unsigned lock_owner;
static uint32_t last_error_counts, last_progress_ms, last_hw_count, last_fifo_bytes;
static unsigned stalled, watch_started;
static struct uart_queue_stats stats;

static int acquire(void)
{
    if (publish_read(&emergency) || x32_cas(&uart_lock, 0, 1))
        return 0;
    lock_owner = uart_hal_core() + 1;
    return 1;
}

static void release(void)
{
    lock_owner = 0;
    publish_write(&uart_lock, 0);
}

static void irqs(void)
{
    uart_hal_write32(BASE + 20u, publish_read(&emergency)
                                     ? 0u
                                     : ((queued && !stalled) ? 1u : 0u) | (paused_rx ? 0u : 2u));
}

int uart_queue_active(void)
{
    return active;
}

int uart_queue_start(void)
{
    /* Legacy CONTROL reads return zero. Set reserved bit 2 only after identifying a controller
     * that supports it. Older buses reject that write. STATUS stays at or below 3 for ROM POST
     * until this handover, so BIOS and U-Boot keep their existing behavior. */
    if (uart_hal_read32(BASE + 8u) != 0x31464155u)
        return -95;
    uart_hal_write8(BASE + 8u, 4);
    if (!(uart_hal_read32(BASE + 4u) & 0x80000000u))
        return -5;
    if (uart_hal_read32(BASE + 12u) != 0x31464155u)
        return -5;
    /* Early boot owns the UART exclusively and no scheduler lock is held. */
    unsigned budget = 1000000;
    while (!(uart_hal_read32(BASE + 4u) & 16u))
        if (!--budget)
            return -5;
    memset(&stats, 0, sizeof stats);
    stats.version = 1;
    stats.bytes = sizeof stats;
    stats.active = 1;
    for (unsigned i = 0; i < UART_REQUESTS; i++)
        publish_write(&requests[i].state, FREE);
    put_at = take_at = queued = rx_put = rx_take = rx_count = paused_rx = stalled = watch_started =
        0;
    last_error_counts = uart_hal_read32(BASE + 24u);
    active = 1;
    irqs();
    return 0;
}

int uart_queue_submit(const unsigned char *data, unsigned n)
{
    if (!active)
        return -95;
    if (n > UART_REQUEST_BYTES || (!data && n))
        return -22;
    if (!n)
        return 0;
    if (!acquire())
        return -11;
    if (stalled)
    {
        release();
        return -5;
    }
    if (queued == UART_REQUESTS)
    {
        stats.backpressure++;
        release();
        return -11;
    }
    struct request *r = requests + put_at;
    if (publish_read(&r->state) != FREE)
    {
        release();
        return -5;
    }
    publish_write(&r->state, WRITING);
    put_at = (put_at + 1) % UART_REQUESTS;
    publish_write(&queued, queued + 1);
    if (queued > stats.high_water)
        stats.high_water = queued;
    irqs();
    release();
    /* The kernel owns this slot and the consumer stops at WRITING. The 4 KiB copy does not hold
     * the UART lock or retain user pointers after the function returns. */
    memcpy(r->data, data, n);
    r->size = n;
    r->position = r->cr = 0;
    stats.logical_bytes += n;
    stats.requests++;
    publish_write(&r->state, READY);
    return (int)n;
}

int uart_queue_service_ready(void)
{
    if (!active || publish_read(&emergency) || publish_read(&uart_lock))
        return 0;
    /* Reading STATUS does not change device state. Quiet RX and error flags, together with
     * unavailable TX space or data, require no UART lock, CAS, W1C or accounting writes. queued
     * is published atomically. Producer arming and level IRQs preserve concurrent arrivals.
     * Recheck ownership before each mutation. */
    uint32_t ready = uart_hal_read32(BASE + 4u);
    return !!((ready & 14u) || (publish_read(&queued) && (ready & 1u)));
}

int uart_queue_service(void)
{
    if (!uart_queue_service_ready() || !acquire())
        return 0;
    uint32_t start = uart_hal_counter();
    unsigned sent = 0, received = 0, polls = 0, freed = 0;
    unsigned budget = 64;
    while (budget && queued && !stalled)
    {
        struct request *r = requests + take_at;
        if (publish_read(&r->state) != READY)
            break;
        unsigned position = r->position, cr = r->cr;
        while (budget && position < r->size)
        {
            uint32_t status = uart_hal_read32(BASE + 4u);
            polls++;
            if (!(status & 1u))
                break;
            unsigned c = r->data[position];
            if (c == '\n' && !cr)
            {
                uart_hal_write8(BASE, '\r');
                cr = 1;
            }
            else
            {
                uart_hal_write8(BASE, (unsigned char)c);
                cr = 0;
                position++;
            }
            sent++;
            budget--;
            if (publish_read(&emergency))
                break;
        }
        r->position = position;
        r->cr = cr;
        if (position == r->size)
        {
            publish_write(&r->state, FREE);
            take_at = (take_at + 1) % UART_REQUESTS;
            publish_write(&queued, queued - 1);
            freed++;
        }
        else
            break;
        if (publish_read(&emergency))
            break;
    }
    unsigned next = rx_put, count = rx_count;
    for (unsigned n = 0; n < 64 && count < sizeof rx_bytes; n++)
    {
        if (!(uart_hal_read32(BASE + 4u) & 2u))
            break;
        rx_bytes[next] = (unsigned char)uart_hal_read32(BASE);
        next = (next + 1) % sizeof rx_bytes;
        count++;
        received++;
        if (publish_read(&emergency))
            break;
    }
    rx_put = next;
    publish_write(&rx_count, count);
    paused_rx = rx_count == sizeof rx_bytes;
    uint32_t errors = uart_hal_read32(BASE + 24u);
    stats.overrun += (uint16_t)((uint16_t)errors - (uint16_t)last_error_counts);
    stats.framing += (uint16_t)((uint16_t)(errors >> 16) - (uint16_t)(last_error_counts >> 16));
    last_error_counts = errors;
    uart_hal_write8(BASE + 8u, 3);
    irqs();
    stats.fifo_bytes += sent;
    stats.rx_bytes += received;
    stats.tx_polls += polls;
    stats.service_calls++;
    if (rx_count > stats.rx_high_water)
        stats.rx_high_water = rx_count;
    uint32_t elapsed = uart_hal_counter() - start;
    stats.service_ticks += elapsed;
    if (elapsed > stats.max_service_ticks)
        stats.max_service_ticks = elapsed;
    release();
    return (sent ? 1 : 0) | (received ? 2 : 0) | (freed ? 4 : 0);
}

int uart_queue_read(void)
{
    if (!active)
        return -95;
    /* An empty RX queue requires only a read-only retry. A later arrival raises the level IRQ. */
    if (!publish_read(&rx_count))
        return -11;
    if (!acquire())
        return -11;
    if (!rx_count)
    {
        release();
        return -11;
    }
    unsigned c = rx_bytes[rx_take];
    rx_take = (rx_take + 1) % sizeof rx_bytes;
    publish_write(&rx_count, rx_count - 1);
    paused_rx = 0;
    irqs();
    release();
    return (int)c;
}

int uart_queue_drain(void)
{
    if (!active)
        return -95;
    if (!acquire())
        return -11;
    int result = stalled ? -5 : !queued && (uart_hal_read32(BASE + 4u) & 16u) ? 0 : -11;
    release();
    return result;
}

void uart_queue_tick(uint32_t now)
{
    if (!active || !acquire())
        return;
    if (!watch_started)
    {
        last_progress_ms = now;
        watch_started = 1;
    }
    uint32_t hw = uart_hal_read32(BASE + 16u) & 65535u;
    int empty = !queued && (uart_hal_read32(BASE + 4u) & 16u);
    if (empty || hw != last_hw_count || stats.fifo_bytes != last_fifo_bytes)
    {
        last_progress_ms = now;
        if (stalled)
        {
            stalled = 0;
            stats.recoveries++;
        }
    }
    else if ((uint32_t)(now - last_progress_ms) >= 2000u && !stalled)
    {
        stalled = 1;
        stats.stalls++;
    }
    last_hw_count = hw;
    last_fifo_bytes = stats.fifo_bytes;
    irqs();
    release();
}

int uart_queue_snapshot(struct uart_queue_stats *out)
{
    if (!out)
        return -22;
    if (!active)
    {
        *out = (struct uart_queue_stats){0};
        out->version = 1;
        out->bytes = sizeof *out;
        return 0;
    }
    if (!acquire())
        return -11;
    *out = stats;
    release();
    return 0;
}

void uart_queue_emergency(const char *message)
{
    unsigned mine = uart_hal_core() + 1;
    publish_write(&emergency, mine);
    if (active)
        uart_hal_write32(BASE + 20u, 0);
    unsigned budget = 100000;
    while (publish_read(&uart_lock) && lock_owner != mine)
        if (!--budget)
            return;
    /* Before reading another byte, each normal consumer checks emergency and releases ownership
     * if set. Accepted RAM remains quarantined and normal processing cannot resume. */
    if (active)
    {
        budget = 1000000;
        while (!(uart_hal_read32(BASE + 4u) & 16u))
            if (!--budget)
                return;
    }
    while (*message)
    {
        unsigned c = (unsigned char)*message++;
        for (unsigned part = 0; part < (c == '\n' ? 2u : 1u); part++)
        {
            budget = 1000000;
            while (!(uart_hal_read32(BASE + 4u) & 1u))
                if (!--budget)
                    return;
            uart_hal_write8(BASE, (unsigned char)(c == '\n' && !part ? '\r' : c));
        }
    }
}
