#ifndef LUME_SOCKET_BUFFER_H
#define LUME_SOCKET_BUFFER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
#include "lume/user_buffer.h"
#define LUME_SOCKET_NATIVE_BUFFER 1
#else
struct lume_user_buffer_token;
#define LUME_SOCKET_NATIVE_BUFFER 0
#endif

struct lume_socket_buffer
{
    struct lume_user_buffer_token *payload, *request, *message;
};

/* A null context denotes a kernel-owned payload. User payloads retain their tokens
 * until every endpoint callback and final metadata copy has returned. */
static inline bool lume_socket_buffer_check(const struct lume_socket_buffer *buffer, size_t offset,
                                            size_t bytes, bool output)
{
#if LUME_SOCKET_NATIVE_BUFFER
    if (!buffer)
        return true;
    if (output &&
        ((buffer->request &&
          !lume_user_buffer_check(buffer->request, 0, (size_t)buffer->request->bytes, true)) ||
         (buffer->message &&
          !lume_user_buffer_check(buffer->message, 0, (size_t)buffer->message->bytes, true))))
        return false;
    return buffer->payload ? lume_user_buffer_check(buffer->payload, offset, bytes, output)
                           : bytes == 0;
#else
    (void)offset;
    (void)bytes;
    (void)output;
    return buffer == NULL;
#endif
}

static inline bool lume_socket_buffer_copy(struct lume_socket_buffer *buffer, uint64_t address,
                                           size_t offset, void *data, size_t bytes, bool output)
{
    if (!lume_socket_buffer_check(buffer, offset, bytes, output))
        return false;
#if LUME_SOCKET_NATIVE_BUFFER
    if (buffer && buffer->payload)
        return lume_user_buffer_copy_at(buffer->payload, offset, data, bytes, output);
#endif
    if (!bytes)
        return true;
    if (output)
        memcpy((void *)(uintptr_t)(address + offset), data, bytes);
    else
        memcpy(data, (const void *)(uintptr_t)(address + offset), bytes);
    return true;
}
#endif
