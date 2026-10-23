#include "lume/dma.h"
#include "lume/mmio.h"
#define PCIE UINT64_C(0x1000120000)
const struct lume_dma_domain lume_rp1_dma_domain = {0, UINT64_C(0x1000000000),
                                                    UINT64_C(0x200000000), UINT64_C(0xffffffffff)};

bool lume_pi5_dma_start(void)
{
    /* Firmware owns the PCIe link. Both xHCI hosts and display DMA share this inbound window. */
    lume_mmio_write32(PCIE + 0x4034, 18);
    lume_mmio_write32(PCIE + 0x4038, 0x10);
    lume_mmio_write32(PCIE + 0x40b4, 1);
    lume_mmio_write32(PCIE + 0x40b8, 0);
    lume_mmio_write32(PCIE + 0x4008,
                      (lume_mmio_read32(PCIE + 0x4008) & ~0xf8000000u) | (18u << 27) | 0x1000);
    return lume_mmio_read32(PCIE + 0x4034) == 18 && lume_mmio_read32(PCIE + 0x4038) == 0x10;
}
