#ifndef LUME_BOOT_PROFILE_H
#define LUME_BOOT_PROFILE_H

enum
{
    LUME_BOOT_DIAGNOSTIC = 0,
    LUME_BOOT_NORMAL = 1,
    LUME_BOOT_RECOVERY = 2
};
#ifndef LUME_BOOT_PROFILE
#define LUME_BOOT_PROFILE LUME_BOOT_DIAGNOSTIC
#endif
/* Diagnostics retain the high-memory and process-isolation checks. Recovery boots a local
 * console without starting network firmware or the network service. */
#endif
