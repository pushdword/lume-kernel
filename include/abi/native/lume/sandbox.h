#ifndef LUME_SANDBOX_ABI_H
#define LUME_SANDBOX_ABI_H
#include <stdint.h>

#define LUME_SANDBOX_VERSION 1u

enum lume_sandbox_operation
{
    LUME_SANDBOX_QUERY,
    LUME_SANDBOX_ENTER,
    LUME_SANDBOX_DIRECTORY_MINT = 16,
    LUME_SANDBOX_DIRECTORY_ACTIVATE,
    LUME_SANDBOX_DIRECTORY_RELEASE,
    LUME_SANDBOX_X11_AUTHORITY = 32
};

#define LUME_SANDBOX_DIRECTORY_LIMIT 128u

enum lume_sandbox_profile
{
    LUME_SANDBOX_NONE,
    LUME_SANDBOX_RENDERER,
    LUME_SANDBOX_UTILITY,
    LUME_SANDBOX_NETWORK,
    LUME_SANDBOX_GPU_SOFTWARE
};

/* QUERY requires zero second and third arguments and returns the current profile.
 * ENTER takes a profile and version. It irreversibly restricts the caller's process.
 * Restrictions survive descriptor inheritance, fork and executable replacement.
 * Reentering the same profile succeeds. Changing an active profile is rejected. */
/* Directory operations take a descriptor and a zero third argument.
 * MINT marks a real directory description in an unrestricted process.
 * ACTIVATE retains that authority in a NETWORK process until RELEASE or exit.
 * Repeated activation of the same directory succeeds without another reference.
 * Descriptor transfer preserves the mark. Other profiles cannot activate it. */
/* X11_AUTHORITY takes a read-only regular file descriptor and a zero third argument.
 * Registration requires an unrestricted process with no other live threads.
 * The GPU profile can reopen only the registered file for reading.
 * Closing the original descriptor does not release this authority. */
#endif
