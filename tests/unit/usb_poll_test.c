#include "lume/usb.h"
#include "lume/dma.h"
#include "lume/graphics_device.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_ARCH_CPU_H
#define LUME_COUNTER_H
#define LUME_MMIO_H

bool lume_graphics_input_owned(void)
{
    return false;
}

void lume_graphics_input(const struct lume_input_event *event)
{
    (void)event;
    abort();
}

static bool worker_busy;
static uint64_t ticks = 1000000;
static uint32_t registers[512], runtime[32], doorbells[16];
static unsigned second_port_reads, event_reads, allocations, diagnostics;

static uint64_t lume_counter_read(void)
{
    return ticks;
}

static void lume_cpu_relax(void)
{
    abort();
}

static void lume_dma_read_barrier(void)
{
}

static uint32_t lume_mmio_read32(uintptr_t address)
{
    uintptr_t first = (uintptr_t)registers;
    assert(address >= first && address - first < sizeof(registers) && !(address & 3));
    if (address == first + 0x410)
        ++second_port_reads;
    return *(uint32_t *)address;
}

static void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    uintptr_t r = (uintptr_t)runtime, d = (uintptr_t)doorbells;
    assert((address >= r && address - r < sizeof(runtime)) ||
           (address >= d && address - d < sizeof(doorbells)));
    assert(!(address & 3));
    *(uint32_t *)address = value;
}

const struct lume_dma_domain lume_rp1_dma_domain = {0};

bool lume_dma_map(const struct lume_dma_domain *domain, uint64_t physical, size_t bytes,
                  uint64_t *bus)
{
    assert(domain == &lume_rp1_dma_domain && bytes == 1);
    *bus = physical;
    return true;
}

bool lume_dma_sync_for_device(const void *data, size_t bytes, enum lume_dma_direction direction)
{
    assert(data && bytes && (direction == LUME_DMA_TO_DEVICE || direction == LUME_DMA_FROM_DEVICE));
    return true;
}

bool lume_dma_sync_for_cpu(const void *data, size_t bytes, enum lume_dma_direction direction)
{
    assert(data && bytes && direction == LUME_DMA_FROM_DEVICE);
    if (bytes == 16)
        ++event_reads;
    return true;
}

bool lume_worker_busy(void)
{
    return worker_busy;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    (void)minimum;
    ++allocations;
    abort();
}

bool lume_page_free(uint64_t page)
{
    (void)page;
    abort();
}

bool lume_pi5_dma_start(void)
{
    abort();
}

void lume_uart_poll_input(void)
{
    abort();
}

void lume_puts(const char *text)
{
    (void)text;
    ++diagnostics;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
    ++diagnostics;
}

_Noreturn void lume_fail(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    abort();
}

#include "../../drivers/usb/xhci/usb.c"

static struct lume_xhci_trb event_ring[256], keyboard_ring[256];
static unsigned char keyboard_report[64];

static void reset(void)
{
    memset(hosts, 0, sizeof(hosts));
    memset(registers, 0, sizeof(registers));
    memset(runtime, 0, sizeof(runtime));
    memset(doorbells, 0, sizeof(doorbells));
    memset(event_ring, 0, sizeof(event_ring));
    memset(keyboard_ring, 0, sizeof(keyboard_ring));
    memset(keyboard_report, 0, sizeof(keyboard_report));
    second_port_reads = event_reads = allocations = diagnostics = 0;
    ticks = 1000000;
    worker_busy = true;
    hosts[0] = (struct host){.op = (uintptr_t)registers,
                             .rt = (uintptr_t)runtime,
                             .doorbells = (uintptr_t)doorbells,
                             .frequency = 1000000,
                             .ports = 2,
                             .events = event_ring,
                             .event_cycle = 1,
                             .running = true,
                             .root_ready = 1};
    registers[0x400 / 4] = 1 | (2u << 10);
}

static void keyboard_event(unsigned index, unsigned key, unsigned modifiers)
{
    struct device *device = &hosts[0].devices[1];
    memset(keyboard_report, 0, sizeof(keyboard_report));
    keyboard_report[0] = (unsigned char)modifiers;
    keyboard_report[2] = (unsigned char)key;
    event_ring[index] = (struct lume_xhci_trb){device->irq_trb, 1u << 24,
                                               (1u << 24) | (3u << 16) | (32u << 10) | 1};
}

static void reports_and_hotplug(void)
{
    reset();
    struct host *host = &hosts[0];
    struct device *device = &host->devices[1];
    *device = (struct device){.slot = 1,
                              .root = 1,
                              .irq_endpoint = 3,
                              .report = keyboard_report,
                              .interrupt = {keyboard_ring, (uintptr_t)keyboard_ring, 0, 1},
                              .irq_trb = 0x9000,
                              .keyboard = true,
                              .pending = true};
    keyboard_event(0, 4, 0);
    assert(lume_usb_getc() == 'a');
    assert(host->completions == 1 && host->event_head == 1 && device->pending);
    assert(device->interrupt.head == 1 && doorbells[1] == 3);
    assert(!host->next_scan && !second_port_reads && !allocations && !diagnostics);

    keyboard_event(1, 6, 1);
    assert(lume_usb_getc() == 3);
    assert(host->completions == 2 && device->interrupt.head == 2);
    assert(!host->next_scan && !second_port_reads);

    device->hid.repeat_key = 4;
    device->hid.modifiers = 0;
    device->hid.repeat_at = 1000;
    assert(lume_usb_getc() == 'a');
    assert(device->hid.repeat_at == 1040 && !second_port_reads);
    device->hid.repeat_key = 0;

    worker_busy = false;
    assert(lume_usb_getc() == -LUME_EAGAIN);
    assert(host->next_scan == 2000 && second_port_reads == 1);
    lume_usb_poll();
    assert(second_port_reads == 1);
    ticks = 2000000;
    lume_usb_poll();
    assert(host->next_scan == 3000 && second_port_reads == 2);
    assert(!allocations && !diagnostics);

    worker_busy = true;
    registers[0x400 / 4] = 0;
    device->hid.repeat_key = 4;
    lume_usb_poll();
    assert(device->disconnected && !device->hid.repeat_key);
    assert(second_port_reads == 2);
}

static void bounded_events(void)
{
    reset();
    for (unsigned index = 0; index < 256; ++index)
        event_ring[index].control = 1;
    lume_usb_poll();
    assert(event_reads == 256 && hosts[0].completions == 256);
    assert(!hosts[0].event_head && !hosts[0].event_cycle);
    assert(!hosts[0].next_scan && !second_port_reads && !allocations && !diagnostics);
    lume_usb_poll();
    assert(event_reads == 257 && hosts[0].completions == 256);
    assert(!second_port_reads);
}

int main(void)
{
    reports_and_hotplug();
    bounded_events();
    puts("PASS_USB_POLL bounded reports, repeat, disconnect and deferred hotplug");
    return 0;
}
