#include "lume/mmio.h"

static void test_write(uintptr_t address, uint32_t value);

#define lume_mmio_write32_sync test_write
#include "../../../examples/ddk/gem.c"
#undef lume_mmio_write32_sync

static unsigned submissions;

static void test_write(uintptr_t address, uint32_t value)
{
    if (physical_rp1 && address == mac_regs + GEM_NETWORK_CONTROL &&
        (value & GEM_CONTROL_TX_START) && tx_pending && ++submissions == 512)
    {
        /* Leave one published descriptor idle to exercise timeout recovery. */
        value &= ~GEM_CONTROL_TX_START;
        lume_value("GEM_TEST_OMITTED_TX_START", submissions);
    }
    lume_mmio_write32_sync(address, value);
}
