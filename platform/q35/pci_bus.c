#include "lume/q35.h"
#include "lume/pci.h"
#include "lume/acpi.h"
#include "lume/hardware.h"
#include "lume/abi.h"
#include <string.h>

#define PCI_DEVICES 192u

struct entry
{
    uint32_t address, identity, id;
    unsigned parent, count;
    bool seen, disabled;
    struct lume_pci_bar bars[6];
};
static struct entry entries[PCI_DEVICES];
static uint32_t root_id;
static bool scanning, started;

uint64_t lume_q35_pci_ecam(uint32_t address, unsigned offset)
{
    const struct lume_acpi *acpi = lume_q35_acpi();
    unsigned bus = address >> 16;
    if (acpi)
        for (unsigned i = 0; i < acpi->segment_count; ++i)
        {
            const struct lume_acpi_segment *s = acpi->segments + i;
            uint64_t start = s->base + ((uint64_t)s->first << 20);
            uint64_t end = s->base + (((uint64_t)s->last + 1) << 20);
            if (!s->segment && bus >= s->first && bus <= s->last && start >= UINT64_C(0x80000000) &&
                end <= UINT64_C(0x100000000))
                return s->base + ((uint64_t)bus << 20) + ((address & 0xff00) << 4) + offset;
        }
    return 0;
}

static int property(const void *context, uint64_t node, const char *name, unsigned kind,
                    unsigned index, struct lume_property *out)
{
    (void)context;
    if (strcmp(name, "pci-address") || kind != LUME_PROPERTY_U32 || index)
        return -LUME_ENOENT;
    *out = (struct lume_property){.kind = kind, .count = 1, .values = {node}};
    return 0;
}

static void hex(char *out, uint32_t value, unsigned digits)
{
    while (digits)
    {
        --digits;
        out[digits] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
}

static bool bridge(const struct entry *e)
{
    return ((lume_q35_pci_read(e->address, 12) >> 16) & 127) == 1;
}

static int discover(unsigned bus, unsigned parent, bool visited[256])
{
    if (visited[bus])
        return 0;
    visited[bus] = true;
    for (unsigned slot = 0; slot < 32; ++slot)
        for (unsigned function = 0; function < 8; ++function)
        {
            uint32_t address = bus << 16 | slot << 11 | function << 8;
            uint32_t identity = lume_q35_pci_read(address, 0);
            if ((identity & 65535) == 65535)
            {
                if (!function)
                    break;
                continue;
            }
            unsigned n = 0, empty = PCI_DEVICES;
            for (; n < PCI_DEVICES; ++n)
            {
                if (!entries[n].identity && empty == PCI_DEVICES)
                    empty = n;
                if (entries[n].identity && entries[n].address == address)
                    break;
            }
            if (n == PCI_DEVICES)
            {
                if (empty == PCI_DEVICES)
                    return -LUME_ENOSPC;
                n = empty;
                entries[n] =
                    (struct entry){.address = address, .identity = identity, .parent = parent};
                entries[n].count = lume_q35_pci_bars(address, entries[n].bars);
            }
            struct entry *e = entries + n;
            /* A retained association at the same BDF must not acquire replacement hardware. */
            if (e->identity == identity)
                e->seen = true;
            if (e->seen && bridge(e))
            {
                uint32_t buses = lume_q35_pci_read(address, 24);
                unsigned secondary = (buses >> 8) & 255, last = (buses >> 16) & 255;
                if (secondary > bus && secondary <= last)
                {
                    int result = discover(secondary, n + 1, visited);
                    if (result)
                        return result;
                }
            }
            if (!function && !(lume_q35_pci_read(address, 12) & 0x00800000))
                break;
        }
    return 0;
}

static bool window(const struct entry *e, unsigned kind, uint64_t *first, uint64_t *end)
{
    *first = kind == LUME_RESOURCE_IOPORT ? 0xc000 : UINT64_C(0xc0000000);
    *end = kind == LUME_RESOURCE_IOPORT ? 0x10000 : UINT64_C(0xfec00000);
    unsigned parent = e->parent, depth = 0;
    while (parent && depth++ < PCI_DEVICES)
    {
        const struct entry *p = entries + parent - 1;
        uint32_t value = lume_q35_pci_read(p->address, kind == LUME_RESOURCE_IOPORT ? 28 : 32);
        uint64_t base, limit;
        if (kind == LUME_RESOURCE_IOPORT)
        {
            uint32_t upper = lume_q35_pci_read(p->address, 48);
            base = (value & 0xf0) << 8;
            limit = (value & 0xf000) | 0xfff;
            if ((value & 15) == 1)
            {
                base |= (uint64_t)(upper & 65535) << 16;
                limit |= upper & 0xffff0000;
            }
        }
        else
        {
            base = (uint64_t)(value & 0xfff0) << 16;
            limit = (value & 0xfff00000) | 0xfffff;
        }
        if (base > limit)
            return false;
        if (base > *first)
            *first = base;
        if (limit + 1 < *end)
            *end = limit + 1;
        parent = p->parent;
    }
    return !parent && *first < *end;
}

static uint64_t allocate(const struct entry *e, const struct lume_resource *r)
{
    uint64_t first, end;
    if (!window(e, r->kind, &first, &end) || r->size > end - first)
        return 0;
    uint64_t base = (first + r->size - 1) & ~(r->size - 1);
    while (base < end && r->size <= end - base)
    {
        uint64_t next = base;
        for (unsigned n = 0; n < PCI_DEVICES; ++n)
        {
            for (unsigned b = 0; b < entries[n].count; ++b)
            {
                const struct lume_resource *p = &entries[n].bars[b].resource;
                if (p->base && p != r && p->kind == r->kind && base < p->base + p->size &&
                    p->base < base + r->size && p->base + p->size > next)
                    next = p->base + p->size;
            }
            /* A sibling bridge decodes its complete aperture, including unused space. */
            unsigned ancestor = e->parent;
            while (ancestor && ancestor != n + 1)
                ancestor = entries[ancestor - 1].parent;
            if (!entries[n].identity || ancestor || !bridge(entries + n))
                continue;
            uint32_t value =
                lume_q35_pci_read(entries[n].address, r->kind == LUME_RESOURCE_IOPORT ? 28 : 32);
            uint64_t start, limit;
            if (r->kind == LUME_RESOURCE_IOPORT)
            {
                uint32_t upper = lume_q35_pci_read(entries[n].address, 48);
                start = (value & 0xf0) << 8;
                limit = (value & 0xf000) | 0xfff;
                if ((value & 15) == 1)
                {
                    start |= (uint64_t)(upper & 65535) << 16;
                    limit |= upper & 0xffff0000;
                }
            }
            else
            {
                start = (uint64_t)(value & 0xfff0) << 16;
                limit = (value & 0xfff00000) | 0xfffff;
            }
            if (start <= limit && base <= limit && start < base + r->size && limit + 1 > next)
                next = limit + 1;
            if (r->kind == LUME_RESOURCE_MMIO)
            {
                value = lume_q35_pci_read(entries[n].address, 36);
                start = (uint64_t)(value & 0xfff0) << 16;
                limit = (value & 0xfff00000) | 0xfffff;
                if ((value & 15) == 1)
                {
                    start |= (uint64_t)lume_q35_pci_read(entries[n].address, 40) << 32;
                    limit |= (uint64_t)lume_q35_pci_read(entries[n].address, 44) << 32;
                }
                if (start <= limit && base <= limit && start < base + r->size &&
                    limit != UINT64_MAX && limit + 1 > next)
                    next = limit + 1;
            }
        }
        if (next == base)
            return base;
        if (next > UINT64_MAX - r->size)
            return 0;
        base = (next + r->size - 1) & ~(r->size - 1);
    }
    return 0;
}

static int publish(struct entry *e)
{
    /* Existing assignments are immutable. Only unassigned endpoint BARs are allocated. */
    uint64_t saved[6];
    for (unsigned i = 0; i < e->count; ++i)
    {
        struct lume_resource *r = &e->bars[i].resource;
        saved[i] = r->base;
        if (!r->base && !bridge(e))
            r->base = allocate(e, r);
        if (!r->base)
        {
            for (unsigned n = 0; n <= i; ++n)
                e->bars[n].resource.base = saved[n];
            return -LUME_ENOSPC;
        }
    }
    struct lume_resource resources[8];
    unsigned count = e->count;
    for (unsigned i = 0; i < count; ++i)
        resources[i] = e->bars[i].resource;
    if (e->identity == UINT32_C(0x11111234))
        resources[count++] =
            (struct lume_resource){.base = 0x1ce, .size = 4, .kind = LUME_RESOURCE_IOPORT};
    char identity[] = "pci,0000:0000", path[] = "/q35/pci/00:00.0";
    hex(identity + 4, e->identity & 65535, 4);
    hex(identity + 9, e->identity >> 16, 4);
    hex(path + 9, e->address >> 16, 2);
    hex(path + 12, (e->address >> 11) & 31, 2);
    hex(path + 15, (e->address >> 8) & 7, 1);
    struct lume_hardware_identity match = {.kind = LUME_HARDWARE_ID_PCI};
    memcpy(match.value, identity, sizeof(identity));
    static const struct lume_firmware_ops firmware = {.property = property};
    struct lume_hardware_description d = {.parent = e->parent ? entries[e->parent - 1].id : root_id,
                                          .origin = LUME_HARDWARE_PCI,
                                          .state = LUME_HARDWARE_DISCOVERED,
                                          .name = identity,
                                          .path = path,
                                          .identities = &match,
                                          .identity_count = 1,
                                          .resources = resources,
                                          .resource_count = count,
                                          .firmware = &firmware,
                                          .firmware_node = e->address};
    int result = lume_hardware_add(&d, &e->id);
    if (result)
        return result;
    uint32_t command = lume_q35_pci_read(e->address, 4) & 65535;
    lume_q35_pci_write(e->address, 4, command & ~3u);
    for (unsigned i = 0; i < e->count; ++i)
        if (!saved[i])
        {
            struct lume_pci_bar *b = e->bars + i;
            lume_q35_pci_write(e->address, b->offset, (uint32_t)b->resource.base | (b->low & 15));
            if (b->wide)
                lume_q35_pci_write(e->address, b->offset + 4, (uint32_t)(b->resource.base >> 32));
        }
    lume_q35_pci_write(e->address, 4, command);
    if (started)
        for (unsigned i = 0; i < lume_driver_count(); ++i)
        {
            const struct lume_driver *driver = lume_driver_at(i);
            if (lume_driver_matches(driver, e->id))
            {
                (void)lume_hardware_bind(e->id, driver);
                break;
            }
        }
    return 0;
}

static bool child(unsigned parent)
{
    for (unsigned n = 0; n < PCI_DEVICES; ++n)
        if (entries[n].identity && entries[n].parent == parent)
            return true;
    return false;
}

static bool descendant(unsigned index, unsigned parent)
{
    for (unsigned depth = 0; entries[index].parent && depth < PCI_DEVICES; ++depth)
    {
        if (entries[index].parent == parent)
            return true;
        index = entries[index].parent - 1;
    }
    return false;
}

static unsigned pcie_slot(const struct entry *e)
{
    if (!(lume_q35_pci_read(e->address, 4) & (1u << 20)))
        return 0;
    unsigned offset = lume_q35_pci_read(e->address, 52) & 255;
    bool visited[64] = {0};
    while (offset >= 64 && offset <= 0xdc && !(offset & 3) && !visited[offset / 4])
    {
        visited[offset / 4] = true;
        uint32_t cap = lume_q35_pci_read(e->address, offset);
        if ((cap & 255) == 0x10 && (cap & (1u << 24)) &&
            (lume_q35_pci_read(e->address, offset + 20) & 0x42) == 0x42)
            return offset;
        offset = (cap >> 8) & 255;
    }
    return 0;
}

static void slots(void)
{
    for (unsigned n = 0; n < PCI_DEVICES; ++n)
    {
        struct entry *port = entries + n;
        if (!port->identity || !port->seen || !bridge(port))
            continue;
        unsigned cap = pcie_slot(port);
        if (!cap)
            continue;
        uint32_t value = lume_q35_pci_read(port->address, cap + 24);
        unsigned control = value & 65535, status = value >> 16;
        if ((status & 0x40) && (control & 0x400) && !child(n + 1))
        {
            /* Insertion into a previously powered-off slot restores its firmware windows. */
            lume_q35_pci_write(port->address, cap + 24, control & ~0x700u);
            continue;
        }
        if (!(status & 1) || !(status & 0x40))
            continue;
        bool ok = true;
        for (unsigned pass = 0; pass < PCI_DEVICES && ok; ++pass)
        {
            bool progress = false;
            for (unsigned i = 0; i < PCI_DEVICES; ++i)
            {
                struct entry *e = entries + i;
                if (!e->identity || e->disabled || !descendant(i, n + 1))
                    continue;
                bool pending = false;
                for (unsigned j = 0; j < PCI_DEVICES; ++j)
                    if (entries[j].identity && !entries[j].disabled && entries[j].parent == i + 1)
                        pending = true;
                if (pending)
                    continue;
                int error = lume_hardware_unbind(e->id);
                if (error)
                {
                    ok = false;
                    break;
                }
                uint32_t command = lume_q35_pci_read(e->address, 4);
                lume_q35_pci_write(e->address, 4, (command & 65535) & ~7u);
                e->disabled = true;
                (void)lume_hardware_state(e->id, LUME_HARDWARE_DISABLED, 0);
                progress = true;
            }
            if (!progress)
                break;
        }
        /* Keep the request pending when a mounted provider or a DMA halt blocks removal. */
        if (ok)
            lume_q35_pci_write(port->address, cap + 24,
                               ((control & ~0x300u) | 0x700u) | (1u << 16));
    }
}

int lume_q35_pci_eject(uint32_t id)
{
    for (unsigned n = 0; n < PCI_DEVICES; ++n)
    {
        struct entry *e = entries + n;
        if (e->id != id || !id)
            continue;
        if (child(n + 1))
            return -LUME_EBUSY;
        int result = lume_hardware_unbind(id);
        if (result)
            return result;
        uint32_t command = lume_q35_pci_read(e->address, 4);
        if (command != UINT32_MAX)
            lume_q35_pci_write(e->address, 4, (command & 65535) & ~7u);
        e->disabled = true;
        return lume_hardware_state(id, LUME_HARDWARE_DISABLED, 0);
    }
    return -LUME_ENOENT;
}

int lume_q35_pci_rescan(void)
{
    if (scanning)
        return -LUME_EBUSY;
    scanning = true;
    for (unsigned n = 0; n < PCI_DEVICES; ++n)
        entries[n].seen = false;
    bool visited[256] = {0};
    int result = discover(0, 0, visited);
    if (!result)
    {
        /* Repeated passes remove children before parents regardless of reused array slots. */
        for (unsigned n = 0; n < PCI_DEVICES; ++n)
            if (entries[n].identity && !entries[n].seen && entries[n].id && child(n + 1))
                (void)lume_hardware_state(entries[n].id, LUME_HARDWARE_ABSENT, -LUME_EIO);
        for (unsigned pass = 0; pass < PCI_DEVICES; ++pass)
        {
            bool removed = false;
            for (unsigned n = 0; n < PCI_DEVICES; ++n)
            {
                struct entry *e = entries + n;
                if (!e->identity || e->seen || child(n + 1))
                    continue;
                int error = e->id ? lume_hardware_unbind(e->id) : 0;
                if (!error && e->id)
                    error = lume_hardware_remove(e->id);
                if (error)
                    (void)lume_hardware_state(e->id, LUME_HARDWARE_ABSENT, error);
                else
                {
                    *e = (struct entry){0};
                    removed = true;
                }
            }
            if (!removed)
                break;
        }
        for (unsigned pass = 0; pass < PCI_DEVICES; ++pass)
        {
            bool added = false;
            for (unsigned n = 0; n < PCI_DEVICES; ++n)
            {
                struct entry *e = entries + n;
                if (!e->seen || e->id || (e->parent && !entries[e->parent - 1].id))
                    continue;
                int error = publish(e);
                if (!error)
                    added = true;
                else if (!result)
                    result = error;
            }
            if (!added)
                break;
        }
    }
    if (!result && started)
        slots();
    scanning = false;
    return result;
}

int lume_q35_pci_start(uint32_t parent)
{
    root_id = parent;
    int result = lume_q35_pci_rescan();
    started = true;
    return result;
}
