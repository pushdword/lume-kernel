#ifndef LUME_LWIP_CC_H
#define LUME_LWIP_CC_H
#include <stdint.h>
#include <stddef.h>
#include "lume/early.h"
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) ((void)0)
#define LWIP_PLATFORM_ASSERT(x) lume_fail(x)
uint32_t lume_net_random(void);
#define LWIP_RAND() lume_net_random()
#endif
