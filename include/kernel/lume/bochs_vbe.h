/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_BOCHS_VBE_H
#define LUME_BOCHS_VBE_H
#include <stdbool.h>
#include <stdint.h>

int lume_bochs_display_register_at(uint32_t address);
int lume_bochs_display_unregister(void);
bool lume_bochs_display_busy(void);
#endif
