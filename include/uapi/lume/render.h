/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_RENDER_ABI_H
#define LUME_RENDER_ABI_H
#include <stdint.h>

#define LUME_RENDER_VERSION 1u
#define LUME_RENDER_CLOEXEC 1u
#define LUME_RENDER_RDWR 2u
#define LUME_RENDER_COMMAND_BYTES (260u * 1024u)
#define LUME_RENDER_ACTION_BASE 256u
#define LUME_RENDER_OPERATION(action) (UINT64_C(1) << ((action) - LUME_RENDER_ACTION_BASE))

enum
{
    LUME_RENDER_OPEN = LUME_RENDER_ACTION_BASE,
    LUME_RENDER_INFO,
    LUME_RENDER_CAPSET_INFO,
    LUME_RENDER_CAPSET,
    LUME_RENDER_RESET,
    LUME_RENDER_CONTEXT,
    LUME_RENDER_RESOURCE_CREATE,
    LUME_RENDER_RESOURCE_EXPORT,
    LUME_RENDER_RESOURCE_CLOSE,
    LUME_RENDER_TRANSFER,
    LUME_RENDER_SUBMIT,
    LUME_RENDER_WAIT,
    LUME_RENDER_RESOURCE_INFO,
    LUME_RENDER_RESOURCE_WAIT,
    LUME_RENDER_PCI,
    LUME_RENDER_PRIME_EXPORT,
    LUME_RENDER_PRIME_IMPORT,
    LUME_RENDER_DISPLAY,
    LUME_RENDER_PRESENT,
    LUME_RENDER_LAST = LUME_RENDER_PRESENT
};

enum
{
    LUME_RENDER_BACKEND_VIRGL = 1
};

/* Supported operations describe the kernel interface. Transport feature bits
 * describe the device. Each operation uses bit (action - ACTION_BASE).
 * Capability data contains the device payload without its transport header. */
struct lume_render_info
{
    uint32_t version, bytes, backend, pci;
    uint64_t transport_features, operations;
    uint32_t scanouts, capsets;
    uint64_t file_id;
};

/* OPEN accepts fd=-1 and returns the descriptor in fd. INFO writes render_info
 * through data. CAPSET_INFO returns the ID, maximum revision and size for index.
 * CAPSET copies the complete payload for the selected index, ID and revision.
 * RESET accepts fd=-1 and requires the driver administration capability. */
struct lume_render_request
{
    uint32_t version, bytes;
    int32_t fd;
    uint32_t flags;
    uint64_t data, length, value;
    uint32_t id, index, revision, reserved;
    uint64_t reserved2;
};

/* Resource handles belong to a render descriptor and its context. Storage is
 * zeroed shared memory. EXPORT returns an independently retained memory descriptor.
 * Closing a handle waits for its context's submitted work before
 * detaching that context. Shared buffers retain backing until their final GPU
 * resource release is acknowledged. */
struct lume_render_resource
{
    uint64_t bytes;
    uint32_t target, format, bind, width, height, depth;
    uint32_t array_size, last_level, samples, flags, handle, reserved;
};

#define LUME_RENDER_TRANSFER_FROM_HOST 1u

struct lume_render_transfer
{
    uint64_t offset;
    uint32_t handle, level, stride, layer_stride;
    uint32_t x, y, z, width, height, depth;
};

struct lume_render_pci
{
    uint32_t domain, bus, device, function;
    uint32_t vendor_product, subsystem, class_revision, reserved;
};

struct lume_render_display
{
    uint32_t x, y, width, height, enabled, flags;
};

/* PRESENT selects a resource rectangle on an enabled display. Its dimensions
 * must match the announced mode. input_fd identifies the caller's acquired
 * graphics session. A zero handle disables the display with an empty rectangle.
 * The display retains its buffer until replacement or disable is acknowledged. */
struct lume_render_present
{
    uint32_t handle, scanout;
    int32_t input_fd;
    uint32_t x, y, width, height, reserved;
};

/* SUBMIT accepts aligned VirGL command words. A nonzero index supplies that many
 * resource handles through value. A zero index includes every owned resource.
 * The returned value is a file-owned fence. WAIT accepts that fence. A zero
 * length polls. A nonzero length waits up to that many milliseconds without
 * retaining the scheduler lock. PRIME_EXPORT returns a retained buffer descriptor
 * in value. PRIME_IMPORT accepts that descriptor in value and returns its handle
 * in id. Importing the same buffer again returns the existing handle without
 * adding another handle reference. Both operations require an owned render descriptor. */

#ifdef __cplusplus
#define LUME_RENDER_ASSERT static_assert
#else
#define LUME_RENDER_ASSERT _Static_assert
#endif
LUME_RENDER_ASSERT(sizeof(struct lume_render_info) == 48, "render information ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_request) == 64, "render request ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_resource) == 56, "render resource ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_transfer) == 48, "render transfer ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_pci) == 32, "render PCI ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_display) == 24, "render display ABI");
LUME_RENDER_ASSERT(sizeof(struct lume_render_present) == 32, "render presentation ABI");
#undef LUME_RENDER_ASSERT
#endif
