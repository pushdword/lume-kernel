#include "lume/q35.h"
#include "lume/pci.h"
#include "lume/acpi.h"
#include "lume/hardware.h"
#include "lume/irq.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
    uint32_t address, registers[64], masks[64];
    bool present, probing[64];
} devices[8];

static uint32_t selected;
static unsigned probes, starts, stops;
static int stop_error;

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    assert(!flags);
}

void lume_worker_device(void (*run)(void *), void *context)
{
    run(context);
}

const struct lume_acpi *lume_q35_acpi(void)
{
    return NULL;
}

uint32_t lume_in32(uint16_t port)
{
    assert(port == 0xcfc);
    for (unsigned i = 0; i < 8; ++i)
        if (devices[i].present && devices[i].address == (selected & 0xffff00))
        {
            unsigned n = (selected & 252) / 4;
            return devices[i].probing[n] ? devices[i].masks[n] : devices[i].registers[n];
        }
    return UINT32_MAX;
}

uint8_t lume_in8(uint16_t port)
{
    assert(port >= 0xcfc && port <= 0xcff);
    return (uint8_t)(lume_in32(0xcfc) >> (8 * (port - 0xcfc)));
}

uint16_t lume_in16(uint16_t port)
{
    assert(port == 0xcfc || port == 0xcfe);
    return (uint16_t)(lume_in32(0xcfc) >> (8 * (port - 0xcfc)));
}

void lume_out32(uint16_t port, uint32_t value)
{
    if (port == 0xcf8)
    {
        selected = value;
        return;
    }
    assert(port == 0xcfc);
    for (unsigned i = 0; i < 8; ++i)
        if (devices[i].present && devices[i].address == (selected & 0xffff00))
        {
            unsigned n = (selected & 252) / 4;
            devices[i].probing[n] = n >= 4 && n < 10 && value == UINT32_MAX;
            if (devices[i].probing[n])
                ++probes;
            else if (n == 1)
                devices[i].registers[n] = (devices[i].registers[n] & 0xffff0000) | (value & 65535);
            else if (i == 0 && n == 0x58 / 4)
            {
                uint32_t status = devices[i].registers[n] >> 16;
                status &= ~(value >> 16);
                if ((value & 0x700) == 0x700)
                {
                    for (unsigned j = 1; j < 8; ++j)
                        if (devices[j].address >> 16 == 1)
                            devices[j].present = false;
                    status &= ~0x40u;
                }
                devices[i].registers[n] = (value & 65535) | status << 16;
            }
            else if (!devices[i].probing[n])
                devices[i].registers[n] = value;
            return;
        }
}

static void write_lane(uint16_t port, uint32_t mask, uint32_t value)
{
    unsigned shift = 8 * (port - 0xcfc);
    for (unsigned i = 0; i < 8; ++i)
        if (devices[i].present && devices[i].address == (selected & 0xffff00))
        {
            unsigned n = (selected & 252) / 4;
            devices[i].registers[n] =
                (devices[i].registers[n] & ~(mask << shift)) | (value << shift);
            return;
        }
}

void lume_out8(uint16_t port, uint8_t value)
{
    assert(port >= 0xcfc && port <= 0xcff);
    write_lane(port, UINT8_MAX, value);
}

void lume_out16(uint16_t port, uint16_t value)
{
    assert(port == 0xcfc || port == 0xcfe);
    write_lane(port, UINT16_MAX, value);
}

static int validate(const struct lume_resource *r)
{
    (void)r;
    return 0;
}

static int start(struct lume_hardware *h, void *context)
{
    (void)h;
    (void)context;
    ++starts;
    return 0;
}

static int stop(struct lume_hardware *h, void *context)
{
    (void)h;
    (void)context;
    ++stops;
    return stop_error;
}

static uint32_t find(const char *path)
{
    struct lume_hardware_info info;
    for (unsigned i = 0; !lume_hardware_info(i, &info); ++i)
        if (!strcmp(info.path, path))
            return info.id;
    return 0;
}

static void endpoint(unsigned n, uint32_t address)
{
    memset(devices + n, 0, sizeof(devices[n]));
    devices[n].address = address;
    devices[n].present = true;
    devices[n].registers[0] = 0x000d1b36;
    devices[n].registers[1] = 7;
    devices[n].registers[4] = 4;
    devices[n].masks[4] = 0xffffc004;
    devices[n].masks[5] = UINT32_MAX;
}

int main(void)
{
    const struct lume_hardware_services services = {malloc, free, validate, NULL};
    assert(!lume_hardware_services(&services));
    struct lume_hardware_description board = {.name = "q35", .path = "/q35", .origin = 1};
    uint32_t root;
    assert(!lume_hardware_add(&board, &root));
    devices[0].present = true;
    devices[0].address = 0x800;
    devices[0].registers[0] = 0x000c1b36;
    devices[0].registers[1] = 7 | 1u << 20;
    devices[0].registers[3] = 1u << 16;
    devices[0].registers[6] = 0x00010100;
    devices[0].registers[7] = 0xd0d0;
    devices[0].registers[8] = 0xc100c100;
    devices[0].registers[13] = 0x40;
    devices[0].registers[16] = 0x10 | 1u << 24;
    devices[0].registers[21] = 0x42;
    devices[0].registers[22] = 0x40u << 16;
    endpoint(1, 0x10000);
    devices[1].registers[0x3c / 4] = 0x010b;
    assert(!lume_q35_pci_start(root));
    uint32_t bridge = find("/q35/pci/00:01.0"), first = find("/q35/pci/01:00.0");
    assert(bridge && first && devices[1].registers[4] == 0xc1000004);
    struct lume_hardware_info info;
    assert(!lume_hardware_get(first, &info) && info.parent == bridge);
    assert(info.resource_count == 2 && info.resources[1].kind == LUME_RESOURCE_IRQ &&
           info.resources[1].base == LUME_PIC_FIRST_VECTOR + 11 &&
           info.resources[1].flags == LUME_IRQ_LEVEL);
    struct lume_property bar;
    assert(!lume_hardware_property_id(first, "pci-bar", LUME_PROPERTY_U64, 0, &bar));
    assert(bar.count == 5 && bar.values[0] == 0xc1000000 && bar.values[1] == 0x4000 &&
           bar.values[2] == LUME_RESOURCE_MMIO && !bar.values[3] && bar.values[4]);
    assert(lume_hardware_property_id(first, "pci-bar", LUME_PROPERTY_U64, 1, &bar) ==
           -LUME_ENOENT);
    const struct lume_hardware_identity match = {LUME_HARDWARE_ID_PCI, 0, "pci,1b36:000d"};
    const struct lume_hardware_ops ops = {.start = start, .stop = stop};
    const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                       .name = "xhci-test",
                                       .matches = &match,
                                       .match_count = 1,
                                       .ops = &ops};
    assert(!lume_driver_register(&driver) && !lume_hardware_bind(first, &driver));
    unsigned previous = probes;
    assert(!lume_q35_pci_rescan() && probes == previous && starts == 1);
    assert(!lume_hardware_property_id(first, "pci-bar", LUME_PROPERTY_U64, 0, &bar) &&
           bar.values[0] == 0xc1000000 && probes == previous);
    endpoint(2, 0x4000);
    devices[2].registers[0x3c / 4] = 0x01ff;
    assert(!lume_q35_pci_rescan());
    uint32_t second = find("/q35/pci/00:08.0");
    assert(second && starts == 2 && devices[2].registers[4] == 0xc0000004);
    assert(!lume_hardware_get(second, &info) && info.resource_count == 1);
    struct lume_hardware_description usb = {.name = "usb",
                                            .path = "/usb/test",
                                            .parent = second,
                                            .origin = LUME_HARDWARE_USB,
                                            .state = LUME_HARDWARE_READY};
    uint32_t usb_id;
    assert(!lume_hardware_add(&usb, &usb_id));
    devices[2].present = false;
    stop_error = -LUME_EIO;
    assert(!lume_q35_pci_rescan());
    assert(!lume_hardware_get(second, &info) && info.state == LUME_HARDWARE_ABSENT &&
           info.driver[0]);
    assert(lume_hardware_bind(second, &driver) == -LUME_EBUSY);
    assert(!lume_hardware_get(usb_id, &info) && info.state == LUME_HARDWARE_ABSENT);
    stop_error = 0;
    assert(!lume_q35_pci_rescan() && !lume_hardware_get(second, &info));
    assert(lume_hardware_unbind(second) == -LUME_EIO && info.state == LUME_HARDWARE_ABSENT);
    assert(!lume_hardware_reference(first));
    assert(lume_q35_pci_eject(first) == -LUME_EBUSY && (devices[1].registers[1] & 7) == 7);
    assert(!lume_hardware_unreference(first));
    devices[0].registers[22] |= 1u << 16;
    stop_error = -LUME_EBUSY;
    assert(!lume_q35_pci_rescan() && !(devices[0].registers[22] & 0x400));
    stop_error = 0;
    assert(!lume_q35_pci_rescan() && (devices[0].registers[22] & 0x700) == 0x700);
    assert(!devices[1].present && stops >= 3);
    assert(!lume_q35_pci_rescan() && !find("/q35/pci/01:00.0"));
    endpoint(1, 0x10000);
    devices[0].registers[22] |= 0x40u << 16;
    assert(!lume_q35_pci_rescan());
    uint32_t replacement = find("/q35/pci/01:00.0");
    assert(replacement && replacement != first && starts == 3);
    assert(!lume_q35_pci_eject(replacement));
    assert(!lume_hardware_get(replacement, &info) && info.state == LUME_HARDWARE_DISABLED);
    assert(!lume_q35_pci_rescan() && starts == 3);
    puts(
        "PCI_BUS_PASS bridge topology, BAR allocation, stable rescan, binding, busy removal, quarantine and slot power");
    return 0;
}
