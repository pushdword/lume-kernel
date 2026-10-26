#define LUME_ARCH_AMD64 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#undef interface
#include "../../../drivers/usb/xhci/usb.c"
#include "lume/hardware.h"
static unsigned char *arena;
static bool used[128], fail_command;
static unsigned live, freed;
static uint64_t ticks;
static struct host *emulated;

uint64_t lume_q35_counter_frequency(void)
{
    return 1000000;
}

uint64_t lume_q35_counter_value(uint64_t raw)
{
    (void)raw;
    return ticks += 1000;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    (void)minimum;
    for (unsigned i = 0; i < 128; ++i)
        if (!used[i])
        {
            used[i] = true;
            ++live;
            return (uintptr_t)arena + i * 4096;
        }
    return 0;
}

bool lume_page_free(uint64_t address)
{
    unsigned index = (unsigned)((address - (uintptr_t)arena) / 4096);
    assert(index < 128 && used[index] && live);
    used[index] = false;
    --live;
    ++freed;
    return true;
}

void lume_uart_poll_input(void)
{
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    abort();
}

bool lume_dma_sync_for_device(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    (void)direction;
    return p && bytes;
}

static void descriptor(struct host *h)
{
    for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
    {
        struct device *d = h->devices + slot;
        if (!d->ep0.trbs || h->waiting_transfer < d->ep0.dma ||
            h->waiting_transfer >= d->ep0.dma + 4096)
            continue;
        unsigned status = (unsigned)((h->waiting_transfer - d->ep0.dma) / 16);
        unsigned previous = status ? status - 1 : 254;
        bool data = ((d->ep0.trbs[previous].control >> 10) & 63) == 3;
        unsigned setup = previous;
        if (data)
            setup = setup ? setup - 1 : 254;
        uint64_t request = d->ep0.trbs[setup].parameter;
        unsigned type = (unsigned)(request >> 24) & 255;
        if (((request >> 8) & 255) == 6 && type == 1)
        {
            const unsigned char bytes[18] = {18,   1,    0,    2, 0, 0, 0, 64, 0x34,
                                             0x12, 0x78, 0x56, 0, 1, 0, 0, 0,  1};
            memcpy(d->buffer, bytes, 18);
        }
        if (((request >> 8) & 255) == 6 && type == 2)
        {
            const unsigned char bytes[25] = {9, 2, 25, 0, 1, 1, 0, 0x80, 0, 9, 4, 0, 0,
                                             1, 3, 1,  1, 0, 7, 5, 0x81, 3, 8, 0, 10};
            memcpy(d->buffer, bytes, 25);
        }
    }
}

bool lume_dma_sync_for_cpu(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    (void)direction;
    struct host *h = emulated;
    if (h && p == &h->events[h->event_head] && (h->waiting_command || h->waiting_transfer))
    {
        struct lume_xhci_trb event = {0};
        event.status = (fail_command ? 5u : 1u) << 24;
        event.control = h->event_cycle;
        if (h->waiting_command)
        {
            event.parameter = h->waiting_command;
            event.control |= 33u << 10;
            unsigned command_type =
                h->command.trbs[(h->waiting_command - h->command.dma) / 16].control >> 10 & 63;
            event.control |=
                h->command.trbs[(h->waiting_command - h->command.dma) / 16].control & 0xff000000;
            if (command_type == 9)
                for (unsigned slot = 1; slot <= MAX_SLOTS; ++slot)
                    if (!h->devices[slot].slot)
                    {
                        event.control |= slot << 24;
                        break;
                    }
        }
        else
        {
            descriptor(h);
            event.parameter = h->waiting_transfer;
            event.control |= 32u << 10 | 1u << 16 | 1u << 24;
        }
        h->events[h->event_head] = event;
        /* One completion per command or transfer. */
        if (h->waiting_command ? h->command_code : h->transfer_code)
            h->events[h->event_head].control ^= 1;
    }
    return p && bytes;
}

static int validate(const struct lume_resource *r)
{
    (void)r;
    return 0;
}

int main(void)
{
    arena = VirtualAlloc((void *)(uintptr_t)0x20000000, 128 * 4096, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    void *registers = VirtualAlloc((void *)(uintptr_t)0xc1000000, 65536, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
    assert(arena && registers);
    const struct lume_hardware_services services = {malloc, free, validate, NULL};
    assert(!lume_hardware_services(&services));
    struct lume_hardware_description controller = {
        .name = "xhci", .path = "/pci/xhci", .origin = LUME_HARDWARE_PCI};
    uint32_t parent;
    assert(!lume_hardware_add(&controller, &parent));
    assert(!lume_usb_configure_pci(0, 0, 0x4000, parent));
    assert(lume_usb_configure_pci(0, (uintptr_t)registers, 65536, parent));
    struct host *h = emulated = hosts;
    h->frequency = 1000000;
    h->base = (uintptr_t)registers;
    h->op = h->base + 0x20;
    h->rt = h->base + 0x1000;
    h->doorbells = h->base + 0x2000;
    h->ports = 1;
    h->context_bytes = 32;
    h->valid = h->running = true;
    h->event_cycle = 1;
    h->events = page(h);
    h->dcbaa = page(h);
    assert(ring_init(h, &h->command));
    assert(enumerate(h, 1, 3, 0, 0, 0, 0, 0));
    assert(keyboard_count == 1 && h->devices[1].inventory);
    uint32_t first = h->devices[1].inventory;
    struct lume_hardware_info info;
    assert(!lume_hardware_get(first, &info) && info.parent == parent &&
           info.state == LUME_HARDWARE_READY && !strcmp(info.identities[0].value, "usb,1234:5678"));
    unsigned before = live, released = freed;
    fail_command = true;
    assert(!remove_root(h, 1) && live == before && freed == released && h->devices[1].disconnected);
    assert(!lume_hardware_get(first, &info));
    fail_command = false;
    assert(remove_root(h, 1) && keyboard_count == 0 && live == 3);
    assert(lume_hardware_get(first, &info) == -LUME_ENOENT);
    assert(enumerate(h, 1, 3, 0, 0, 0, 0, 0));
    uint32_t replacement = h->devices[1].inventory;
    assert(replacement && replacement != first);
    before = live;
    assert(!lume_usb_stop_host(0) && live == before);
    *(uint32_t *)(uintptr_t)(h->op + 4) = 1;
    assert(lume_usb_stop_host(0) && !live && !keyboard_count);
    assert(lume_hardware_get(replacement, &info) == -LUME_ENOENT);
    assert(!lume_hardware_remove(parent));
    assert(VirtualFree(registers, 0, MEM_RELEASE) && VirtualFree(arena, 0, MEM_RELEASE));
    puts(
        "USB_LIFECYCLE_PASS descriptor enumeration, inventory, reconnect IDs, disable-slot failure and DMA halt retention");
    return 0;
}
