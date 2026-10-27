#include "lume/console_output.h"
#include <assert.h>
#include <stdio.h>
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

#define LUME_ARCH_CPU_H

static void lume_cpu_relax_ordered(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

static unsigned relaxations, completions;

static void lume_cpu_relax(void)
{
    ++relaxations;
}

static void lume_memory_complete(void)
{
    ++completions;
}

#include "../../src/core/console_output.c"

#define THREADS 4u
#define WRITES 4000u
static unsigned in_tx, records;
static uint32_t expected;
static unsigned ready, go;

static THREAD_RESULT producer(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
        lume_cpu_relax_ordered();
    for (unsigned i = 0; i < WRITES; ++i)
    {
        uint64_t ticket = lume_console_serial_reserve();
        assert(ticket >> 32 == 1);
        lume_console_serial_wait(ticket);
        assert(!__atomic_fetch_add(&in_tx, 1, __ATOMIC_RELAXED));
        assert((uint32_t)ticket == expected++);
        unsigned char buffer[128];
        for (unsigned j = 0; j < sizeof(buffer); ++j)
            buffer[j] = (unsigned char)(id ^ i ^ j);
        for (unsigned j = 0; j < sizeof(buffer); ++j)
            assert(buffer[j] == (unsigned char)(id ^ i ^ j));
        ++records;
        assert(__atomic_fetch_sub(&in_tx, 1, __ATOMIC_RELAXED) == 1);
        lume_console_serial_unlock(ticket);
    }
    return 0;
}

static void stress(uint32_t seed)
{
    ready = go = in_tx = records = 0;
    serial.next = serial.serving = expected = seed;
    test_thread threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, producer, (void *)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(&threads[i], NULL, producer, (void *)(uintptr_t)i));
#endif
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != THREADS)
        lume_cpu_relax_ordered();
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 20000) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[i]));
#else
        assert(!pthread_join(threads[i], NULL));
#endif
    }
    assert(records == THREADS * WRITES && !in_tx && serial.next == serial.serving);
}

#define LUME_MMIO_H
#define LUME_BOARD_H
#define LUME_BOARD_RP1_UART_BASE UINT64_C(0x1c00030000)
static unsigned full_polls, register_reads, display_calls, usb_calls, checkpoints;
static unsigned raw_checkpoints, raw_reads;

void lume_pi5_uart_rx_checkpoint(void)
{
    ++raw_checkpoints;
}

int lume_pi5_uart_rx_read(void)
{
    ++raw_reads;
    return -11;
}

uint64_t lume_pi5_uart_rx_dropped(void)
{
    return 0;
}

static unsigned char wire[256], rendered[128];
static size_t wire_bytes, rendered_bytes;

static uint32_t lume_mmio_read32_relaxed(uintptr_t address)
{
    assert(address == LUME_BOARD_RP1_UART_BASE + 0x18);
    ++register_reads;
    if (full_polls)
    {
        --full_polls;
        return (1u << 4) | (1u << 5);
    }
    return 1u << 4;
}

static void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    assert(address == LUME_BOARD_RP1_UART_BASE && value <= 255 && wire_bytes < sizeof(wire));
    wire[wire_bytes++] = (unsigned char)value;
}

int lume_usb_getc(void)
{
    ++usb_calls;
    return -11;
}

void lume_display_write(const char *data, size_t bytes)
{
    assert(bytes <= sizeof(rendered));
    ++display_calls;
    rendered_bytes = bytes;
    for (size_t i = 0; i < bytes; ++i)
        rendered[i] = (unsigned char)data[i];
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes <= 64);
    ++checkpoints;
}

#include "../../drivers/serial/pl011/uart.c"

static void transport(void)
{
    full_polls = 100;
    lume_uart_transmit("one\r\ntwo\n", 9);
    assert(relaxations == 100 && register_reads == 109 && wire_bytes == 9);
    assert(raw_checkpoints == 109 && raw_reads == 0);
    assert(!memcmp(wire, "one\r\ntwo\n", 9));
    assert(!display_calls && !usb_calls && !checkpoints);
    lume_uart_render("image\n", 6);
    assert(display_calls == 1 && rendered_bytes == 6 && !memcmp(rendered, "image\n", 6));
    assert(wire_bytes == 9 && !usb_calls && !checkpoints);
    lume_putc('\n');
    assert(wire_bytes == 11 && wire[9] == '\r' && wire[10] == '\n');
}

int main(void)
{
    assert(!lume_console_serial_reserve() && !lume_console_serial_lock());
    lume_console_serial_wait(0);
    lume_console_serial_unlock(0);
    assert(!serial.next && !serial.serving);
    lume_console_output_start();
    uint64_t ticket = lume_console_serial_lock();
    assert(ticket == UINT64_C(0x100000000));
    lume_console_serial_unlock(ticket);
    stress(0);
    stress(UINT32_MAX - 100);
    transport();
    assert(completions == 32002);
    puts("CONSOLE_OUTPUT_PASS boot, FIFO, exclusion, wrap and isolated TX records=32000");
    return 0;
}
