/* X32 is the default architecture when no Lume target is selected. */
#ifndef LUME_ARCH_CPU_H
#define LUME_ARCH_CPU_H
#include "contract.h"
#include <stdint.h>
#if defined(LUME_ARCH_X32) && defined(LUME_ARCH_R32)
#error "Select exactly one Lume architecture"
#endif
#if defined(LUME_ARCH_R32)
#include "r32/cpu.h"
#else
#include "x32/cpu.h"
#endif
#endif
