/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_TYPES_H
#define LUME_LINUX_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef unsigned int gfp_t;
#define __iomem
#define __maybe_unused __attribute__((unused))
#define SMP_CACHE_BYTES 64
#define GFP_KERNEL 1u
#define GFP_ATOMIC 2u
#endif
