#include "lume/user_buffer.h"
#include "lume/early.h"
#include "lume/mmu.h"
#include "lume/vm.h"
#include "lume/smp.h"
#include <string.h>

static struct lume_user_buffer_token *tokens;
static unsigned token_count;

static uint64_t user_root(unsigned owner)
{
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    return lume_mmu_task_view_root(owner, lume_mmu_jit_active(owner));
#else
    return lume_mmu_task_root(owner);
#endif
}

static bool view_matches(const struct lume_user_buffer_token *token)
{
    if (user_root(token->owner) != token->root)
        return false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    return lume_mmu_root_active(token->root);
#else
    return true;
#endif
}

static bool resident(unsigned pid, uint64_t first, size_t bytes, bool write, bool remote,
                     uint64_t physical[2])
{
    if (!lume_smp_owned() || !pid || pid > LUME_TASK_LIMIT || bytes > LUME_USER_RESIDENT_MAX ||
        first > UINTPTR_MAX || bytes > UINTPTR_MAX - first)
        return false;
    unsigned owner = pid - 1;
    uint64_t root = user_root(owner);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (remote)
        root = lume_mmu_task_view_root(owner, false);
#endif
    if (!root || !lume_vm_buffer_ready(owner, first, bytes))
        return false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (!remote && !lume_mmu_root_active(root))
        return false;
#endif
    if (!bytes)
        return true;
    if (!lume_mmu_translate(root, first, write, true, false, physical))
        return false;
    size_t part = 4096u - (size_t)(first & 4095u);
    return bytes <= part ||
           lume_mmu_translate(root, first + part, write, true, false, physical + 1);
}

bool lume_user_buffer_resident(unsigned pid, uint64_t first, size_t bytes, bool write,
                               uint64_t physical[2])
{
    return resident(pid, first, bytes, write, false, physical);
}

bool lume_user_buffer_resident_remote(unsigned pid, uint64_t first, size_t bytes, bool write,
                                      uint64_t physical[2])
{
    return resident(pid, first, bytes, write, true, physical);
}

int lume_user_buffer_acquire(struct lume_user_buffer_token *token, unsigned pid, uint64_t first,
                             size_t bytes, bool write)
{
    *token = (struct lume_user_buffer_token){0};
    if (!pid || pid > LUME_TASK_LIMIT || bytes > LUME_FILE_IO_MAX || first > UINTPTR_MAX ||
        bytes > UINTPTR_MAX - first || !lume_smp_owned())
        return -LUME_EFAULT;
    if (token_count == LUME_TASK_LIMIT)
        return -LUME_ENOMEM;
    if (!lume_user_buffer(first, bytes, write))
        return -LUME_EFAULT;
    token->owner = pid - 1;
    token->first = first;
    token->bytes = bytes;
    token->write = write;
    token->root = user_root(token->owner);
    if (!token->root || !view_matches(token) || !lume_vm_buffer_ready(token->owner, first, bytes))
        return -LUME_EFAULT;
    uint64_t address = first;
    size_t remaining = bytes;
    while (remaining)
    {
        uint64_t physical;
        if (!lume_mmu_translate(token->root, address, write, true, false, &physical))
            return -LUME_EFAULT;
        token->physical[token->pages++] = physical & ~UINT64_C(4095);
        size_t part = 4096u - (size_t)(address & 4095u);
        if (part > remaining)
            part = remaining;
        remaining -= part;
        address += part;
    }
    if (token_count == LUME_TASK_LIMIT)
        return -LUME_ENOMEM;
    token->valid = token->registered = true;
    token->next = tokens;
    tokens = token;
    ++token_count;
    return 0;
}

bool lume_user_buffer_prepare(struct lume_user_buffer_token *token, unsigned pid, uint64_t first,
                              size_t bytes, bool write)
{
    return lume_user_buffer_acquire(token, pid, first, bytes, write) == 0;
}

bool lume_user_buffer_check(const struct lume_user_buffer_token *token, size_t offset, size_t bytes,
                            bool to_user)
{
    if (!lume_smp_owned() || !token->registered || !token->valid || offset > token->bytes ||
        bytes > token->bytes - offset || (to_user && !token->write) || !view_matches(token) ||
        !lume_vm_buffer_ready(token->owner, token->first + offset, bytes))
        return false;
    uint64_t address = token->first + offset;
    size_t remaining = bytes;
    unsigned page = (unsigned)(((token->first & 4095u) + offset) / 4096u);
    while (remaining)
    {
        uint64_t physical;
        if (!lume_mmu_translate(token->root, address, to_user, true, false, &physical) ||
            (physical & ~UINT64_C(4095)) != token->physical[page++])
            return false;
        size_t part = 4096u - (size_t)(address & 4095u);
        if (part > remaining)
            part = remaining;
        remaining -= part;
        address += part;
    }
    return true;
}

static void copy_checked(const struct lume_user_buffer_token *token, size_t offset, void *buffer,
                         size_t bytes, bool to_user)
{
    /* The complete range is checked before any byte is copied. Physical aliases
     * cannot fault and no callback or allocator runs before this copy completes. */
    size_t remaining = bytes;
    size_t done = 0;
    unsigned page = (unsigned)(((token->first & 4095u) + offset) / 4096u);
    uint64_t address = token->first + offset;
    while (remaining)
    {
        size_t within = (size_t)(address & 4095u);
        size_t part = 4096u - within;
        if (part > remaining)
            part = remaining;
        void *physical = (void *)(uintptr_t)(token->physical[page++] + within);
        if (to_user)
            memcpy(physical, (unsigned char *)buffer + done, part);
        else
            memcpy((unsigned char *)buffer + done, physical, part);
        done += part;
        remaining -= part;
        address += part;
    }
}

bool lume_user_buffer_copy_at(struct lume_user_buffer_token *token, size_t offset, void *buffer,
                              size_t bytes, bool to_user)
{
    if (!lume_user_buffer_check(token, offset, bytes, to_user))
        return false;
    copy_checked(token, offset, buffer, bytes, to_user);
    return true;
}

bool lume_user_buffer_copy_strided(struct lume_user_buffer_token *token, void *buffer,
                                   size_t row_bytes, size_t pitch, unsigned rows)
{
    if (!rows || !row_bytes || pitch < row_bytes || row_bytes > token->bytes ||
        (rows > 1 && pitch > (token->bytes - row_bytes) / (rows - 1)))
        return false;
    size_t span = (size_t)(rows - 1) * pitch + row_bytes;
    if (!lume_user_buffer_check(token, 0, span, false))
        return false;
    if (pitch == row_bytes)
    {
        copy_checked(token, 0, buffer, span, false);
        return true;
    }
    for (unsigned row = 0; row < rows; ++row)
        copy_checked(token, (size_t)row * pitch, (unsigned char *)buffer + (size_t)row * row_bytes,
                     row_bytes, false);
    return true;
}

bool lume_user_buffer_copy(struct lume_user_buffer_token *token, void *buffer, size_t bytes,
                           bool to_user)
{
    return lume_user_buffer_copy_at(token, 0, buffer, bytes, to_user);
}

void lume_user_buffer_release(struct lume_user_buffer_token *token)
{
    if (!token->registered)
        return;
    struct lume_user_buffer_token **position = &tokens;
    while (*position && *position != token)
        position = &(*position)->next;
    if (!*position || !token_count)
        lume_fail("user buffer token ownership");
    *position = token->next;
    --token_count;
    token->registered = token->valid = false;
    token->next = NULL;
}

void lume_user_buffer_invalidate(unsigned owner, uint64_t first, uint64_t bytes)
{
    if (!bytes)
        return;
    for (struct lume_user_buffer_token *token = tokens; token; token = token->next)
        if (token->owner == owner && token->bytes &&
            (first <= token->first ? token->first - first < bytes
                                   : first - token->first < token->bytes))
            token->valid = false;
}

void lume_user_buffer_invalidate_root(unsigned owner)
{
    for (struct lume_user_buffer_token *token = tokens; token; token = token->next)
        if (token->owner == owner)
            token->valid = false;
}
