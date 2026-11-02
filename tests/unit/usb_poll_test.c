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

static bool graphics;
static struct lume_input_event pointer;
static unsigned pointers;

bool lume_graphics_input_owned(void)
{
    return graphics;
}

void lume_graphics_input(const struct lume_input_event *event)
{
    assert(graphics && event->type == LUME_INPUT_POINTER);
    pointer = *event;
    ++pointers;
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
    event_ring[index] = (struct lume_xhci_trb){device->channels[0].trb, 1u << 24,
                                               (1u << 24) | (3u << 16) | (32u << 10) | 1};
}

static void reports_and_hotplug(void)
{
    reset();
    struct host *host = &hosts[0];
    struct device *device = &host->devices[1];
    *device = (struct device){.slot = 1,
                              .root = 1,
                              .inputs = 1,
                              .channels = {{.endpoint = 3,
                                            .packet = 8,
                                            .report = keyboard_report,
                                            .ring = {keyboard_ring, (uintptr_t)keyboard_ring, 0, 1},
                                            .trb = 0x9000,
                                            .keyboard = true,
                                            .pending = true}}};
    keyboard_event(0, 4, 0);
    assert(lume_usb_getc() == 'a');
    assert(host->completions == 1 && host->event_head == 1 && device->channels[0].pending);
    assert(device->channels[0].ring.head == 1 && doorbells[1] == 3);
    assert(!host->next_scan && !second_port_reads && !allocations && !diagnostics);

    keyboard_event(1, 6, 1);
    assert(lume_usb_getc() == 3);
    assert(host->completions == 2 && device->channels[0].ring.head == 2);
    assert(!host->next_scan && !second_port_reads);

    device->channels[0].hid.repeat_key = 4;
    device->channels[0].hid.modifiers = 0;
    device->channels[0].hid.repeat_at = 1000;
    assert(lume_usb_getc() == 'a');
    assert(device->channels[0].hid.repeat_at == 1040 && !second_port_reads);
    device->channels[0].hid.repeat_key = 0;

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
    device->channels[0].hid.repeat_key = 4;
    lume_usb_poll();
    assert(device->disconnected && !device->channels[0].hid.repeat_key);
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

static void composite_reports(void)
{
    reset();
    struct host *host = hosts;
    struct device *device = &host->devices[1];
    static struct lume_xhci_trb mouse_ring[256];
    static uint8_t mouse_report[64];
    *device = (struct device){.slot = 1, .root = 1, .inputs = 2};
    device->channels[0] = (struct input){.endpoint = 7,
                                         .packet = 16,
                                         .report = keyboard_report,
                                         .ring = {keyboard_ring, (uintptr_t)keyboard_ring, 0, 1},
                                         .trb = 0x9000,
                                         .keyboard = true,
                                         .pending = true};
    device->channels[1] = (struct input){.endpoint = 3,
                                         .packet = 8,
                                         .report = mouse_report,
                                         .ring = {mouse_ring, (uintptr_t)mouse_ring, 0, 1},
                                         .trb = 0xa000,
                                         .mouse = true,
                                         .pending = true};
    keyboard_report[2] = 4;
    event_ring[0] =
        (struct lume_xhci_trb){0x9000, (13u << 24) | 8, (1u << 24) | (7u << 16) | (32u << 10) | 1};
    assert(lume_usb_getc() == 'a' && !host->errors && device->channels[0].pending);
    assert(keyboard_ring[0].status == 16 && doorbells[1] == 7);

    graphics = true;
    pointers = 0;
    mouse_report[0] = 5;
    mouse_report[1] = 9;
    mouse_report[2] = (uint8_t)-7;
    mouse_report[3] = 0xff;
    event_ring[1] =
        (struct lume_xhci_trb){0xa000, (13u << 24) | 4, (1u << 24) | (3u << 16) | (32u << 10) | 1};
    events(host);
    assert(pointers == 1 && pointer.x == 9 && pointer.y == -7 && pointer.buttons == 5);
    assert(!device->channels[1].pending && device->channels[0].pending && !host->errors);
    arm_input(host, device, device->channels + 1);
    assert(mouse_ring[0].status == 8 && doorbells[1] == 3);
    uint64_t trb = device->channels[1].trb;
    event_ring[2] =
        (struct lume_xhci_trb){trb + 16, 1u << 24, (1u << 24) | (3u << 16) | (32u << 10) | 1};
    events(host);
    assert(pointers == 1 && device->channels[1].pending);
    event_ring[3] =
        (struct lume_xhci_trb){trb, (13u << 24) | 6, (1u << 24) | (3u << 16) | (32u << 10) | 1};
    events(host);
    assert(pointers == 1 && !device->channels[1].pending && !host->errors);
    arm_input(host, device, device->channels + 1);
    event_ring[4] = (struct lume_xhci_trb){device->channels[1].trb, (13u << 24) | 9,
                                           (1u << 24) | (3u << 16) | (32u << 10) | 1};
    events(host);
    assert(pointers == 1 && device->disconnected && host->errors == 1);
    graphics = false;
}

int main(void)
{
    reports_and_hotplug();
    bounded_events();
    composite_reports();
    puts("PASS_USB_POLL bounded reports, composite endpoints, short packets and deferred hotplug");
    return 0;
}
