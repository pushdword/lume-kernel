#ifndef LUME_SDIO_H
#define LUME_SDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Polling SDIO bus. The backend owns deadlines and PIO, never DMA. */
#define LUME_SDIO_RESPONSE (2u << 16)
#define LUME_SDIO_CHECKS ((1u << 19) | (1u << 20))
struct lume_sdio_bus {
    bool (*command)(unsigned, uint32_t, uint32_t, uint32_t *);
    bool (*read53)(uint32_t, unsigned char *, size_t);
    void (*delay_us)(unsigned);
    bool (*write53)(uint32_t, const unsigned char *, size_t);
};
struct lume_sdio_identity {
    uint32_t ocr, rca, signature;
    uint16_t vendor, device;
    unsigned functions;
    unsigned char cccr, version, capability, interface[7], clock_csr;
};
bool lume_sdio_direct(const struct lume_sdio_bus *, bool write, unsigned function,
                      uint32_t address, unsigned char *byte);
bool lume_sdio_identify(const struct lume_sdio_bus *, struct lume_sdio_identity *);
bool lume_sdio_backplane_read(const struct lume_sdio_bus *, uint32_t, uint32_t *);
bool lume_sdio_backplane_write(const struct lume_sdio_bus *, uint32_t, uint32_t);
bool lume_sdio_memory(const struct lume_sdio_bus *,bool,uint32_t,void *,size_t);
const struct lume_sdio_bus *lume_wifi_bus(void);
bool lume_wifi_transfer_clock(void);
bool lume_wifi_probe(uint64_t counter_hz, bool bcm2712d0);
void lume_wifi_stop(void);
#endif
