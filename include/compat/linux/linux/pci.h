/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_PCI_H
#define LUME_LINUX_PCI_H
#include <linux/device.h>
#include <linux/dma-mapping.h>

#define PCIBIOS_SUCCESSFUL 0x00
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
#define PCIBIOS_BAD_REGISTER_NUMBER 0x87
#define PCIBIOS_SET_FAILED 0x88

#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400

#define PCI_STD_NUM_BARS 6
#define IORESOURCE_IO 0x00000100
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_PREFETCH 0x00002000
#define IORESOURCE_MEM_64 0x00100000

struct lume_linux_pci_bar
{
    u64 start, length;
    unsigned long flags;
};

struct pci_dev
{
    struct device dev;
    u16 vendor, device;
    unsigned int irq;
    struct lume_linux_pci_bar bars[PCI_STD_NUM_BARS];
    unsigned long requested_bars;
    bool enabled, bus_master, managed_enabled;
};

int lume_linux_pci_read_config(const struct pci_dev *, int, unsigned, u32 *);
int lume_linux_pci_write_config(const struct pci_dev *, int, unsigned, u32);
int pci_enable_device(struct pci_dev *);
int pci_enable_device_mem(struct pci_dev *);
int pcim_enable_device(struct pci_dev *);
void pci_disable_device(struct pci_dev *);
void pci_set_master(struct pci_dev *);
void pci_clear_master(struct pci_dev *);
int pci_request_region(struct pci_dev *, unsigned bar, const char *name);
int pci_request_regions(struct pci_dev *, const char *name);
int pci_request_selected_regions(struct pci_dev *, unsigned long bars, const char *name);
int pci_request_mem_regions(struct pci_dev *, const char *name);
void pci_release_region(struct pci_dev *, unsigned bar);
void pci_release_regions(struct pci_dev *);
void pci_release_selected_regions(struct pci_dev *, unsigned long bars);
unsigned long pci_select_bars(const struct pci_dev *, unsigned long flags);
void *pci_iomap(struct pci_dev *, unsigned bar, unsigned long maxlen);
void *pci_iomap_range(struct pci_dev *, unsigned bar, unsigned long offset, unsigned long maxlen);
void *pcim_iomap(struct pci_dev *, unsigned bar, unsigned long maxlen);
void pci_iounmap(struct pci_dev *, void *);
void pcim_iounmap(struct pci_dev *, void *);

static inline void *pci_ioremap_bar(struct pci_dev *device, unsigned bar)
{
    return pci_iomap(device, bar, 0);
}

static inline int pci_set_dma_mask(struct pci_dev *device, u64 mask)
{
    return dma_set_mask(&device->dev, mask);
}

static inline int pci_set_consistent_dma_mask(struct pci_dev *device, u64 mask)
{
    return dma_set_coherent_mask(&device->dev, mask);
}

static inline void pci_set_drvdata(struct pci_dev *device, void *data)
{
    dev_set_drvdata(&device->dev, data);
}

static inline void *pci_get_drvdata(struct pci_dev *device)
{
    return dev_get_drvdata(&device->dev);
}

static inline u64 pci_resource_start(const struct pci_dev *device, unsigned bar)
{
    return bar < PCI_STD_NUM_BARS ? device->bars[bar].start : 0;
}

static inline u64 pci_resource_len(const struct pci_dev *device, unsigned bar)
{
    return bar < PCI_STD_NUM_BARS ? device->bars[bar].length : 0;
}

static inline u64 pci_resource_end(const struct pci_dev *device, unsigned bar)
{
    u64 start = pci_resource_start(device, bar), length = pci_resource_len(device, bar);
    return length && start <= UINT64_MAX - (length - 1) ? start + length - 1 : 0;
}

static inline unsigned long pci_resource_flags(const struct pci_dev *device, unsigned bar)
{
    return bar < PCI_STD_NUM_BARS ? device->bars[bar].flags : 0;
}

static inline int pci_read_config_byte(const struct pci_dev *device, int where, u8 *value)
{
    if (!value)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    u32 raw;
    int result = lume_linux_pci_read_config(device, where, 1, &raw);
    *value = (u8)raw;
    return result;
}

static inline int pci_read_config_word(const struct pci_dev *device, int where, u16 *value)
{
    if (!value)
        return PCIBIOS_BAD_REGISTER_NUMBER;
    u32 raw;
    int result = lume_linux_pci_read_config(device, where, 2, &raw);
    *value = (u16)raw;
    return result;
}

static inline int pci_read_config_dword(const struct pci_dev *device, int where, u32 *value)
{
    return lume_linux_pci_read_config(device, where, 4, value);
}

static inline int pci_write_config_byte(const struct pci_dev *device, int where, u8 value)
{
    return lume_linux_pci_write_config(device, where, 1, value);
}

static inline int pci_write_config_word(const struct pci_dev *device, int where, u16 value)
{
    return lume_linux_pci_write_config(device, where, 2, value);
}

static inline int pci_write_config_dword(const struct pci_dev *device, int where, u32 value)
{
    return lume_linux_pci_write_config(device, where, 4, value);
}
#endif
