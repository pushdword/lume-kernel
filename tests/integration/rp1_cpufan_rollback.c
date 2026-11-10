/* SPDX-License-Identifier: GPL-2.0-only */
#define lume_devfs_register_group fan_test_reject_group
#include "../../drivers/pwm/rp1/cpufan.c"

int fan_test_reject_group(struct lume_devfs_group *group)
{
    (void)group;
    return -LUME_EIO;
}
