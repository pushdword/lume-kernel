#ifndef LUME_FILE_MODE_ABI_H
#define LUME_FILE_MODE_ABI_H
#include <stdint.h>
#define LUME_FILE_MODE_VERSION 1u

enum
{
    LUME_FILE_MODE_PATH,
    LUME_FILE_MODE_FD,
    LUME_FILE_MODE_OPEN,
    LUME_FILE_MODE_MKDIR
};

struct lume_file_mode_request
{
    uint32_t version, bytes;
    uint64_t path;
    int32_t fd;
    uint32_t mode, flags, descriptor_flags, uid, gid;
};

_Static_assert(sizeof(struct lume_file_mode_request) == 40, "file mode request ABI");
#endif
