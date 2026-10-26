#ifndef LUME_MODULE_ABI_H
#define LUME_MODULE_ABI_H
#include <stdint.h>

#define LUME_MODULE_ABI_VERSION 1u

enum
{
    LUME_MODULE_LIST = 1,
    LUME_MODULE_LOAD,
    LUME_MODULE_UNLOAD,
    LUME_MODULE_KERNEL,
    LUME_MODULE_INSPECT
};

struct lume_module_info
{
    uint32_t id, references, drivers, devices;
    uint64_t bytes;
    char name[64];
    unsigned char kernel_build[20];
    uint32_t reserved;
};

/* LOAD copies bytes from the caller before parsing. Other operations require zero input. */
struct lume_module_request
{
    uint32_t version, operation, index, id;
    uint64_t file, bytes;
    struct lume_module_info info;
};

_Static_assert(sizeof(struct lume_module_info) == 112, "module info layout");
_Static_assert(sizeof(struct lume_module_request) == 144, "module request layout");
#endif
