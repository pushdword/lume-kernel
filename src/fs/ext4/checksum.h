#ifndef LUME_EXT4_CHECKSUM_H
#define LUME_EXT4_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/* Reflected Castagnoli CRC with a caller-provided seed and no final XOR. */
uint32_t lume_ext4_crc32c(uint32_t, const unsigned char *, size_t);

#endif
