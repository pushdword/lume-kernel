#include "lume/uart_rx.h"
#include "lume/board.h"
#include "lume/console_output.h"
#include "lume/abi.h"
#include "lume/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE test_thread;
#define THREAD_RESULT DWORD WINAPI
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_t test_thread;
#define THREAD_RESULT void *
#endif

#define LUME_MMIO_H
#define LUME_COUNTER_H
#define LUME_SMP_H

static _Thread_local unsigned cpu;
static uint64_t flags[2] = {0x80, 0x80}, ticks;
static uint32_t pmr[2] = {0xff, 0xff}, priorities = 0x80808080;
static unsigned saves, restores, pmr_reads, pmr_writes;
static bool owned, in_interrupt;
static uint32_t pending_token = 1023;
static unsigned device_pending, eois;
static unsigned fifo[32], fifo_head, fifo_tail, hardware_drops;
static unsigned input_next, input_limit, steps, full_polls, sent;
static unsigned char wire[256];
static unsigned release_at;
static uint64_t held_ticket;
static unsigned receive_wait, receive_full, stop_at;
static bool disable_at_stop, stopped;
static bool stop_test, collector_entered, collector_release, stop_waiting, stop_returned;

static void host_yield(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void step(void);
static uint32_t lume_mmio_read32_relaxed(uintptr_t);
static void lume_mmio_write32_relaxed(uintptr_t, uint32_t);
static void lume_mmio_write32_sync(uintptr_t, uint32_t);
static void lume_mmio_write32(uintptr_t, uint32_t);

static unsigned lume_cpu_index(void)
{
    return cpu;
}

static bool lume_smp_owned(void)
{
    return owned;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

static uint64_t lume_counter_read_relaxed(void)
{
    return ticks;
}

uint64_t lume_arch_irq_save(void)
{
    __atomic_fetch_add(&saves, 1, __ATOMIC_RELAXED);
    uint64_t value = flags[cpu];
    flags[cpu] |= 0x80;
    return value;
}

void lume_arch_irq_restore(uint64_t value)
{
    __atomic_fetch_add(&restores, 1, __ATOMIC_RELAXED);
    flags[cpu] = value;
}

void lume_memory_complete(void)
{
}

_Noreturn void lume_fail(const char *message)
{
    fputs(message, stderr);
    abort();
}

void lume_cpu_relax(void)
{
    if (stop_test && cpu == 1)
    {
        __atomic_store_n(&stop_waiting, true, __ATOMIC_RELEASE);
        host_yield();
        return;
    }
    step();
}

void lume_cpu_relax_ordered(void)
{
    step();
}

#include "../../platform/raspberrypi5/uart_rx.c"

static void step(void)
{
    ++steps;
    assert(steps < 100000);
    ticks += 1000;
    if (input_next < input_limit)
    {
        if (fifo_head - fifo_tail == 32)
            ++hardware_drops;
        else
            fifo[fifo_head++ % 32] = input_next & 255u;
        ++input_next;
        device_pending = 1u << 25;
    }
    if (release_at && steps == release_at)
    {
        lume_console_serial_unlock(held_ticket);
        held_ticket = 0;
    }
    if (stop_at && steps == stop_at)
    {
        unsigned previous_cpu = cpu;
        cpu = 1;
        if (disable_at_stop)
            lume_pi5_uart_rx_enable(false);
        else
            lume_pi5_uart_rx_irq_mode(false);
        cpu = previous_cpu;
        stopped = true;
        assert(serial_scopes && !irq_ready);
        if (disable_at_stop)
            input_limit = input_next;
    }
    if (!cpu && !in_interrupt && irq_ready && !(flags[0] & 0x80) &&
        pmr[0] > LUME_BOARD_UART_IRQ_PRIORITY && device_pending)
    {
        uint64_t before = flags[0];
        flags[0] |= 0x80;
        in_interrupt = true;
        pending_token = LUME_BOARD_UART_IRQ;
        unsigned token;
        assert(lume_pi5_uart_rx_interrupt(&token) && token == LUME_BOARD_UART_IRQ);
        receive_wait += sent == 0;
        receive_full += full_polls != 0;
        in_interrupt = false;
        flags[0] = before;
    }
}

static uint32_t lume_mmio_read32_relaxed(uintptr_t address)
{
    if (address == LUME_BOARD_GIC_CPU_BASE + 4)
    {
        ++pmr_reads;
        return pmr[cpu];
    }
    if (address == LUME_BOARD_GIC_CPU_BASE + 0xc)
    {
        unsigned token = pending_token;
        pending_token = 1023;
        return token;
    }
    if (address == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x504)
        return priorities;
    if (address == LUME_BOARD_RP1_BASE + 0x108108)
        return device_pending;
    if (address == LUME_BOARD_RP1_UART_BASE + 0x18)
    {
        unsigned tx = 0;
        if (collecting && stop_test && !collector_entered)
        {
            __atomic_store_n(&collector_entered, true, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&collector_release, __ATOMIC_ACQUIRE))
                host_yield();
        }
        assert(!collecting || __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || stop_test);
        if (!collecting)
        {
            step();
            if (full_polls)
            {
                --full_polls;
                tx = 1u << 5;
            }
        }
        return (fifo_head == fifo_tail ? 1u << 4 : 0) | tx;
    }
    assert(address == LUME_BOARD_RP1_UART_BASE && collecting && fifo_head != fifo_tail);
    return fifo[fifo_tail++ % 32];
}

static void lume_mmio_write32_relaxed(uintptr_t address, uint32_t value)
{
    assert(address == LUME_BOARD_RP1_UART_BASE + 4 && !value);
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    if (address == LUME_BOARD_GIC_CPU_BASE + 4)
    {
        ++pmr_writes;
        pmr[cpu] = value;
    }
    else if (address == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x504)
        priorities = value;
    else if (address == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x1a0)
        assert(value == (1u << 5));
    else if (address == LUME_BOARD_RP1_UART_BASE + 0x44)
        assert(value == ((1u << 4) | (1u << 6) | (15u << 7)));
    else if (address == LUME_BOARD_RP1_BASE + 0x10886c)
    {
        assert(value == (1u << 2));
        device_pending = 0;
    }
    else if (address == LUME_BOARD_GIC_CPU_BASE + 0x10)
    {
        assert(value == LUME_BOARD_UART_IRQ);
        ++eois;
    }
    else
        assert(address == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0xf00 && value == (UINT32_C(2) << 24));
}

static void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    assert(address == LUME_BOARD_RP1_UART_BASE && sent < sizeof(wire));
    wire[sent++] = (unsigned char)value;
}

int lume_usb_getc(void)
{
    assert(false);
    return -LUME_EAGAIN;
}

void lume_display_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
    assert(false);
}

void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
    assert(false);
}

#include "../../drivers/serial/pl011/uart.c"

static bool copied_transmit(bool interrupt_mode, uint32_t priority, unsigned stop, bool disable)
{
    cpu = 0;
    assert(!serial_scopes && !lock_requested && !lock_window);
    flags[0] = 0x80;
    pmr[0] = priority;
    fifo_head = fifo_tail = hardware_drops = 0;
    input_next = input_limit = steps = full_polls = sent = 0;
    receive_wait = receive_full = 0;
    stop_at = stop;
    disable_at_stop = disable;
    stopped = false;
    owned = true;
    lume_pi5_uart_rx_enable(true);
    if (interrupt_mode)
        lume_pi5_uart_rx_irq_mode(true);
    else
        lume_pi5_uart_rx_lock_enter();
    assert(!interrupt_mode || (lock_window && !(flags[0] & 0x80)));
    struct lume_console_output output = {.transmit = lume_uart_transmit, .bytes = 128};
    for (unsigned i = 0; i < 64; ++i)
    {
        output.data[i * 2] = '\r';
        output.data[i * 2 + 1] = '\n';
    }
    held_ticket = lume_console_serial_reserve();
    output.ticket = lume_console_serial_reserve();
    release_at = 12;
    input_limit = 96;
    full_polls = 64;
    owned = false;
    lume_pi5_uart_rx_lock_leave();
    assert(flags[0] & 0x80);
    lume_console_output_send(&output);
    assert(sent == 128 && !memcmp(wire, output.data, sent) && !held_ticket);
    unsigned received = 0;
    for (int value; (value = lume_pi5_uart_rx_read()) >= 0; ++received)
        assert(value == (int)(received & 255));
    printf("RX_TX received=%u expected=%u hardware_drops=%u irq=%u steps=%u\n", received,
           input_limit, hardware_drops, eois, steps);
    bool progress = disable || (received == input_limit && !hardware_drops);
    assert(!stop || stopped);
    if (interrupt_mode && priority > LUME_BOARD_UART_IRQ_PRIORITY && !stop)
        progress &= receive_wait >= release_at && receive_full > 0;
    lume_pi5_uart_rx_enable(false);
    return progress && flags[0] == 0x80 && pmr[0] == priority && !serial_scopes &&
           !lock_requested && !lock_window;
}

static void nested_scopes(void)
{
    cpu = 0;
    flags[0] = 0x3c0;
    pmr[0] = 0xa0;
    owned = false;
    fifo_head = fifo_tail = 0;
    device_pending = 0;
    lume_pi5_uart_rx_enable(true);
    lume_pi5_uart_rx_irq_mode(true);
    assert(!lock_window);
    lume_console_receive_begin();
    lume_console_receive_begin();
    assert(serial_scopes == 2 && lock_window && pmr[0] == 0x80 && flags[0] == 0x340);
    unsigned before_saves = saves, before_restores = restores;
    unsigned before_reads = pmr_reads, before_writes = pmr_writes;
    for (unsigned i = 0; i < 10000; ++i)
        lume_pi5_uart_rx_lock_enter();
    printf("LOCK_WINDOW spins=10000 irq_saves=%u irq_restores=%u pmr_reads=%u pmr_writes=%u\n",
           saves - before_saves, restores - before_restores, pmr_reads - before_reads,
           pmr_writes - before_writes);
    assert(saves - before_saves == 1 && restores - before_restores == 1 &&
           pmr_reads == before_reads && pmr_writes == before_writes);
    assert(serial_scopes == 2 && lock_requested);
    lume_pi5_uart_rx_lock_leave();
    lume_console_receive_end();
    assert(serial_scopes == 1 && lock_window && pmr[0] == 0x80 && flags[0] == 0x340);
    cpu = 1;
    lume_pi5_uart_rx_irq_mode(false);
    cpu = 0;
    lume_pi5_uart_rx_checkpoint();
    assert(serial_scopes == 1 && !lock_window && pmr[0] == 0xa0 && flags[0] == 0x3c0);
    lume_pi5_uart_rx_irq_mode(true);
    assert(lock_window && pmr[0] == 0x80 && flags[0] == 0x340);
    lume_console_receive_end();
    assert(!serial_scopes && !lock_window && pmr[0] == 0xa0 && flags[0] == 0x3c0);
    lume_pi5_uart_rx_enable(false);

    cpu = 1;
    before_saves = saves;
    before_reads = pmr_reads;
    before_writes = pmr_writes;
    lume_console_receive_begin();
    lume_pi5_uart_rx_lock_enter();
    lume_pi5_uart_rx_lock_wait();
    lume_pi5_uart_rx_lock_leave();
    lume_console_receive_end();
    assert(saves == before_saves && pmr_reads == before_reads && pmr_writes == before_writes &&
           !serial_scopes && !lock_requested);
    cpu = 0;
}

static void ring_overflow(void)
{
    unsigned expected = 0;
    uint64_t before = lume_pi5_uart_rx_dropped();
    fifo_head = fifo_tail = 0;
    lume_pi5_uart_rx_enable(true);
    for (unsigned i = 0; i < 16385;)
    {
        unsigned count = 16385 - i < 32 ? 16385 - i : 32;
        for (unsigned j = 0; j < count; ++j)
            fifo[fifo_head++ % 32] = (i + j) & 255;
        i += count;
        lume_pi5_uart_rx_collect();
    }
    for (int value; (value = lume_pi5_uart_rx_read()) >= 0; ++expected)
        assert(value == (int)(expected & 255));
    assert(expected == 16384 && lume_pi5_uart_rx_dropped() == before + 1);
    lume_pi5_uart_rx_enable(false);
}

static THREAD_RESULT collect_thread(void *argument)
{
    (void)argument;
    cpu = 0;
    lume_pi5_uart_rx_collect();
    return 0;
}

static THREAD_RESULT disable_thread(void *argument)
{
    (void)argument;
    cpu = 1;
    lume_pi5_uart_rx_enable(false);
    __atomic_store_n(&stop_returned, true, __ATOMIC_RELEASE);
    return 0;
}

static void collector_quiescence(void)
{
    cpu = 0;
    owned = false;
    fifo_head = 1;
    fifo_tail = 0;
    fifo[0] = 0x5a;
    lume_pi5_uart_rx_enable(true);
    lume_pi5_uart_rx_irq_mode(true);
    stop_test = true;
    test_thread collector, stopper;
#ifdef _WIN32
    collector = CreateThread(NULL, 0, collect_thread, NULL, 0, NULL);
    assert(collector);
#else
    assert(!pthread_create(&collector, NULL, collect_thread, NULL));
#endif
    while (!__atomic_load_n(&collector_entered, __ATOMIC_ACQUIRE))
        host_yield();
#ifdef _WIN32
    stopper = CreateThread(NULL, 0, disable_thread, NULL, 0, NULL);
    assert(stopper);
#else
    assert(!pthread_create(&stopper, NULL, disable_thread, NULL));
#endif
    while (!__atomic_load_n(&stop_waiting, __ATOMIC_ACQUIRE))
        host_yield();
    assert(!__atomic_load_n(&stop_returned, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&enabled, __ATOMIC_SEQ_CST));
    __atomic_store_n(&collector_release, true, __ATOMIC_RELEASE);
#ifdef _WIN32
    assert(WaitForSingleObject(collector, 20000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(stopper, 20000) == WAIT_OBJECT_0);
    assert(CloseHandle(collector) && CloseHandle(stopper));
#else
    assert(!pthread_join(collector, NULL) && !pthread_join(stopper, NULL));
#endif
    assert(stop_returned && !collecting && !irq_ready);
    assert(lume_pi5_uart_rx_read() == 0x5a);
    assert(lume_pi5_uart_rx_read() == -LUME_EAGAIN);
    stop_test = false;
}

int main(void)
{
    lume_console_output_start();
    if (!copied_transmit(true, 0xff, 0, false) || !copied_transmit(true, 0x20, 0, false) ||
        !copied_transmit(false, 0xff, 0, false) || !copied_transmit(true, 0xff, 20, false) ||
        !copied_transmit(true, 0xff, 20, true))
        return 1;
    nested_scopes();
    ring_overflow();
    collector_quiescence();
    puts("PASS UART RX progresses during serial wait and copied TX.");
    return 0;
}
