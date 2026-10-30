#define LUME_BOOT_TRIAL_HOST_TEST 1
#include "lume/boot_trial.h"
#include "../../src/storage/boot_storage.c"

static bool trial_running, trial_match = true, arm_success = true;
static unsigned arm_calls;

bool lume_boot_trial_supported(void)
{
    return true;
}

bool lume_boot_trial_running(void)
{
    return trial_running;
}

bool lume_boot_trial_arm(void)
{
    ++arm_calls;
    return arm_success;
}

bool lume_boot_trial_matches(unsigned bytes, unsigned crc)
{
    (void)bytes;
    (void)crc;
    return trial_match;
}

void lume_boot_trial_accept(void)
{
    trial_running = false;
}

#define LUME_BOOT_STORAGE_TRIAL_TEST 1
#include "boot_storage_test.c"
