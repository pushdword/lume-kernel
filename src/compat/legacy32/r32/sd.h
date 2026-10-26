#ifndef LUME_SD_H
#define LUME_SD_H
#include <stdint.h>
#define LUME_SPI_STATUS 12u
#define LUME_SPI_ABORT 20u
#define LUME_SPI_RX_BLOCK 24u
#define LUME_SPI_RX_POP 28u
#define LUME_SPI_CAP_RX_BLOCK 0x80000000u
#define LUME_SPI_RX_START 0x80000000u
#define LUME_SPI_CAP_TX_BLOCK 0x40000000u
#define LUME_SPI_TX_START 0x40000000u
#define LUME_SPI_RX_MAGIC 0x31464253u
#define LUME_SPI_BUSY 1u
#define LUME_SPI_DONE 8u
#define LUME_SPI_CRC_OK 16u
#define LUME_SPI_WORDS(status) (((status) >> 8) & 255u)

enum lume_sd_status
{
    LUME_SD_ABSENT = 0,
    LUME_SD_READY = 1,
    LUME_SD_TRANSPORT = 2,
    LUME_SD_RESPONSE = 3,
    LUME_SD_TIMEOUT = 4,
    LUME_SD_CSD = 5,
    LUME_SD_CRC = 6,
    LUME_SD_WRITE_ERROR = 7
};

/* Last group: UINT32_MAX means the number committed is unknown after error. */
extern uint32_t lume_sd_partial_blocks;

struct lume_sd_card
{
    uint32_t status, blocks, capacity_mib, high_capacity, mbr_signature;
};

int lume_sd_identify(uintptr_t base, struct lume_sd_card *card);
int lume_sd_read(uintptr_t base, const struct lume_sd_card *card, uint32_t block,
                 unsigned char *data);
/* Success confirms that every sector was validated. The caller must discard the output after an
 * error. */
int lume_sd_read_blocks(uintptr_t, const struct lume_sd_card *, uint32_t, uint32_t,
                        unsigned char *);
int lume_sd_decode_csd(const unsigned char *csd, struct lume_sd_card *card);
int lume_sd_write(uintptr_t, const struct lume_sd_card *, uint32_t, const unsigned char *);
/* CMD25 writes contiguous sector groups. Success requires status verification and complete
 * readback. */
int lume_sd_write_blocks(uintptr_t, const struct lume_sd_card *, uint32_t, uint32_t,
                         const unsigned char *);
int lume_sd_sync(uintptr_t);
#endif
