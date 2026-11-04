#ifndef LUME_V3D_BITOPS_H
#define LUME_V3D_BITOPS_H
#include <stdint.h>
typedef uint32_t u32;
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(high, low) ((UINT32_MAX << (low)) & (UINT32_MAX >> (31u - (high))))
#endif
