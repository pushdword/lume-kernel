#ifndef LUME_SD_H
#define LUME_SD_H
#include <stdbool.h>
#include <stdint.h>
#define LUME_SD_MAX_SECTORS 32u
bool lume_sd_start(uint64_t counter_frequency);
bool lume_sd_configure(uint64_t base, uint64_t size, uint32_t irq);
bool lume_sd_stop(void);
bool lume_sd_read(uint64_t sector, void *data);
bool lume_sd_write(uint64_t sector, const void *data);
/* Groups complete synchronously. A failed write may have changed earlier sectors. */
bool lume_sd_read_blocks(uint64_t sector, unsigned count, void *data);
bool lume_sd_write_blocks(uint64_t sector, unsigned count, const void *data);
uint64_t lume_sd_sectors(void);
bool lume_sd_allow_volume(uint64_t first, uint64_t count);
bool lume_sd_allow_boot_volume(uint64_t first, uint64_t count);
void lume_sd_stats(void);
#endif
