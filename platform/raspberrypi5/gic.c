#include "lume/early.h"
#include "lume/irq.h"
#include "lume/mmio.h"

#define GICD UINT64_C(0x107fff9000)
#define GICC UINT64_C(0x107fffa000)
static bool started;

uint64_t lume_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags)::"memory");
    return flags;
}

void lume_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0; isb" ::"r"(flags) : "memory");
}

static int configure(unsigned irq, unsigned trigger)
{
    uint64_t group = GICD + 0x80 + 4 * (irq / 32);
    lume_mmio_write32_sync(group, lume_mmio_read32_relaxed(group) | (1u << (irq % 32)));
    uint64_t priority = GICD + 0x400 + (irq & ~3u);
    unsigned shift = (irq & 3) * 8;
    lume_mmio_write32_sync(priority, (lume_mmio_read32_relaxed(priority) & ~(0xffu << shift)) |
                                         (0x80u << shift));
    if (irq >= 32)
    {
        uint64_t target = GICD + 0x800 + (irq & ~3u);
        lume_mmio_write32_sync(target, (lume_mmio_read32_relaxed(target) & ~(0xffu << shift)) |
                                           (1u << shift));
    }
    uint64_t config = GICD + 0xc00 + 4 * (irq / 16);
    shift = (irq % 16) * 2;
    lume_mmio_write32_sync(config, (lume_mmio_read32_relaxed(config) & ~(2u << shift)) |
                                       (trigger == LUME_IRQ_EDGE ? 2u << shift : 0));
    lume_mmio_write32_sync(GICD + 0x280 + 4 * (irq / 32), 1u << (irq % 32));
    return 0;
}

static void enable(unsigned irq, bool on)
{
    lume_mmio_write32_sync(GICD + (on ? 0x100 : 0x180) + 4 * (irq / 32), 1u << (irq % 32));
}

unsigned lume_irq_ack(void)
{
    return lume_mmio_read32_relaxed(GICC + 0xc);
}

void lume_irq_eoi(unsigned token)
{
    lume_mmio_write32_sync(GICC + 0x10, token);
}
static const struct lume_irq_controller operations = {configure, enable, lume_irq_eoi,
                                                      lume_irq_save, lume_irq_restore};

void lume_pi5_irq_secondary(void)
{
    lume_mmio_write32_sync(GICD + 0x80, UINT32_MAX);
    lume_mmio_write32_sync(GICD + 0x400, 0x80808080);
    lume_mmio_write32_sync(GICD + 0x41c, 0x80808080);
    lume_mmio_write32_sync(GICD + 0x100, 1u << 30 | 1u);
    lume_mmio_write32_sync(GICC + 4, 0xff);
    lume_mmio_write32_sync(GICC + 8, 0);
    lume_mmio_write32_sync(GICC, 1);
}

bool lume_pi5_irq_start(void)
{
    if (started)
        return true;
    uint64_t flags = lume_irq_save();
    unsigned lines = 32 * ((lume_mmio_read32_relaxed(GICD + 4) & 31) + 1);
    if (lines > LUME_IRQ_LIMIT)
        lines = LUME_IRQ_LIMIT;
    lume_mmio_write32_sync(GICD, 0);
    for (unsigned i = 0; i < (lines + 31) / 32; ++i)
    {
        lume_mmio_write32_sync(GICD + 0x180 + 4 * i, UINT32_MAX);
        lume_mmio_write32_sync(GICD + 0x280 + 4 * i, UINT32_MAX);
    }
    lume_mmio_write32_sync(GICC + 4, 0xff);
    lume_mmio_write32_sync(GICC + 8, 0);
    lume_mmio_write32_sync(GICC, 1);
    lume_mmio_write32_sync(GICD, 1);
    configure(0, LUME_IRQ_EDGE);
    enable(0, true);
    started = lume_irq_controller_start(&operations, lines) == 0;
    lume_irq_restore(flags);
    return started;
}
