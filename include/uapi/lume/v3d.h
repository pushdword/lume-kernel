/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_V3D_ABI_H
#define LUME_V3D_ABI_H
#include <stdint.h>

#define LUME_V3D_VERSION 1u
#define LUME_V3D_HANDLES 4096u
#define LUME_V3D_FLUSH_CACHE 1u
#define LUME_V3D_EXPORT_READONLY 2u
#define LUME_V3D_FORMAT_XRGB8888 UINT32_C(0x34325258)
#define LUME_V3D_FORMAT_XBGR8888 UINT32_C(0x34324258)
#define LUME_V3D_FORMAT_RGB565 UINT32_C(0x36314752)
#define LUME_V3D_FORMAT_BGR565 UINT32_C(0x36314742)

enum
{
    LUME_V3D_PARAMETER,
    LUME_V3D_CREATE,
    LUME_V3D_INFO,
    LUME_V3D_CLOSE,
    LUME_V3D_EXPORT,
    LUME_V3D_IMPORT,
    LUME_V3D_SUBMIT_CL,
    LUME_V3D_SUBMIT_TFU,
    LUME_V3D_SUBMIT_CSD,
    LUME_V3D_WAIT,
    LUME_V3D_SYNC_CREATE,
    LUME_V3D_SYNC_DESTROY,
    LUME_V3D_SYNC_WAIT,
    LUME_V3D_SYNC_SIGNAL,
    LUME_V3D_SYNC_EXPORT,
    LUME_V3D_SYNC_IMPORT,
    LUME_V3D_LAYOUT,
    LUME_V3D_SYNC_RESET
};

/* Buffer addresses belong to one render description. Jobs retain their buffers
 * until completion or acknowledged reset. Handles and descriptors are distinct. */
struct lume_v3d_request
{
    uint32_t version, bytes, operation, flags;
    uint32_t handle, address, size, count;
    uint64_t handles, value, timeout;
    uint32_t registers[16];
    uint32_t in_sync[2], out_sync, reserved;
};

/* CL registers contain bin start/end, render start/end, tile allocation
 * address/size and tile state address. TFU registers contain icfg, iia, iis,
 * ica, iua, ioa, ios, four coefficients and ioc. CSD uses seven registers. */
#endif
