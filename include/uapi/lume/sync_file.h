/*
 *  sync abstraction
 *  Copyright 2015-2016 Collabora Ltd.
 *
 *  Based on the implementation from the Android Open Source Project,
 *
 *  Copyright 2012 Google, Inc
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 *  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 *  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *  OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LUME_SYNC_FILE_H
#define LUME_SYNC_FILE_H
#include <stdint.h>

#define LUME_SYNC_FILE_MERGE UINT32_C(0xc0303e03)
#define LUME_SYNC_FILE_INFO UINT32_C(0xc0383e04)

struct lume_sync_merge_data
{
    char name[32];
    int32_t fd2, fence;
    uint32_t flags, pad;
};

struct lume_sync_fence_info
{
    char object[32], driver[32];
    int32_t status;
    uint32_t flags;
    uint64_t timestamp_ns;
};

struct lume_sync_file_info
{
    char name[32];
    int32_t status;
    uint32_t flags, count, pad;
    uint64_t fences;
};
#endif
