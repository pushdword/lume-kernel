#ifndef LUME_SD_H
#define LUME_SD_H
#include <stdbool.h>
#include <stdint.h>
bool lume_sd_start(uint64_t counter_frequency);
bool lume_sd_read(uint64_t sector, void *data);
bool lume_sd_write(uint64_t sector, const void *data);
uint64_t lume_sd_sectors(void);
bool lume_sd_allow_volume(uint64_t first, uint64_t count);
void lume_sd_stats(void);
#endif
