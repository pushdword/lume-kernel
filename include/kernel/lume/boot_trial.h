#ifndef LUME_BOOT_TRIAL_H
#define LUME_BOOT_TRIAL_H
#include <stdbool.h>
#include <stddef.h>

#if defined(__aarch64__) || defined(LUME_BOOT_TRIAL_HOST_TEST)
void lume_boot_trial_context(const void *, const void *, size_t);
bool lume_boot_trial_supported(void);
bool lume_boot_trial_running(void);
bool lume_boot_trial_arm(void);
bool lume_boot_trial_progress(void);
bool lume_boot_trial_matches(unsigned, unsigned);
void lume_boot_trial_accept(void);
#else
static inline bool lume_boot_trial_supported(void)
{
    return false;
}

static inline bool lume_boot_trial_running(void)
{
    return false;
}

static inline bool lume_boot_trial_arm(void)
{
    return false;
}

static inline bool lume_boot_trial_matches(unsigned bytes, unsigned crc)
{
    (void)bytes;
    (void)crc;
    return false;
}

static inline void lume_boot_trial_accept(void)
{
}
#endif
#endif
