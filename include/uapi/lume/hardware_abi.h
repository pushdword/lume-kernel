#ifndef LUME_HARDWARE_ABI_H
#define LUME_HARDWARE_ABI_H
#include <stdint.h>

#define LUME_HARDWARE_VERSION 1u
#define LUME_HARDWARE_NAME 64u
#define LUME_HARDWARE_PATH 160u
#define LUME_HARDWARE_IDS 4u
#define LUME_HARDWARE_ID_BYTES 96u
#define LUME_HARDWARE_RESOURCES 16u

enum lume_hardware_origin
{
    LUME_HARDWARE_BOARD = 1,
    LUME_HARDWARE_DT,
    LUME_HARDWARE_ACPI,
    LUME_HARDWARE_PCI,
    LUME_HARDWARE_USB,
    LUME_HARDWARE_VIRTUAL
};

enum lume_hardware_state
{
    LUME_HARDWARE_DISCOVERED,
    LUME_HARDWARE_DISABLED,
    LUME_HARDWARE_UNSUPPORTED,
    LUME_HARDWARE_READY,
    LUME_HARDWARE_ABSENT,
    LUME_HARDWARE_DEFERRED,
    LUME_HARDWARE_FAILED
};

enum lume_hardware_identity_kind
{
    LUME_HARDWARE_ID_DT = 1,
    LUME_HARDWARE_ID_ACPI,
    LUME_HARDWARE_ID_PCI,
    LUME_HARDWARE_ID_USB,
    LUME_HARDWARE_ID_BOARD
};

struct lume_hardware_identity
{
    uint32_t kind, reserved;
    char value[LUME_HARDWARE_ID_BYTES];
};

struct lume_hardware_resource_info
{
    uint64_t base, size;
    uint32_t kind, flags, domain, reserved;
};

/* Local identifiers remain unique for the current boot. All reserved fields are zero. */
struct lume_hardware_info
{
    uint32_t version, id, parent, origin;
    uint32_t state, identity_count, resource_count, reserved;
    int32_t error;
    uint32_t references;
    char name[LUME_HARDWARE_NAME];
    char path[LUME_HARDWARE_PATH];
    char driver[LUME_HARDWARE_NAME];
    struct lume_hardware_identity identities[LUME_HARDWARE_IDS];
    struct lume_hardware_resource_info resources[LUME_HARDWARE_RESOURCES];
};

enum lume_hardware_operation
{
    LUME_HARDWARE_LIST,
    LUME_HARDWARE_GET,
    LUME_HARDWARE_BIND,
    LUME_HARDWARE_UNBIND,
    LUME_HARDWARE_READ_REGISTER,
    LUME_HARDWARE_DRIVER,
    LUME_HARDWARE_RESCAN,
    LUME_HARDWARE_EJECT
};

/* Register widths and resource extents use bytes. */
struct lume_hardware_request
{
    uint32_t version, operation, id, index;
    uint32_t resource, width, reserved[2];
    uint64_t offset, value;
    char driver[LUME_HARDWARE_NAME];
    struct lume_hardware_info info;
};

_Static_assert(sizeof(struct lume_hardware_info) == 1256, "hardware information ABI");
_Static_assert(sizeof(struct lume_hardware_request) == 1368, "hardware request ABI");
#endif
