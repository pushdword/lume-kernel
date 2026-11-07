#ifndef LUME_USER_BUFFER_H
#define LUME_USER_BUFFER_H
#include "lume/abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_USER_BUFFER_PAGES ((LUME_FILE_IO_MAX + 4095u) / 4096u + 1u)

struct lume_user_buffer_token
{
    struct lume_user_buffer_token *next;
    uint64_t first, bytes, root;
    uint64_t physical[LUME_USER_BUFFER_PAGES];
    unsigned owner, pages;
    bool write, valid, registered;
};

/* The scheduler lock protects token registration and every final copy. Preparation
 * may resolve faults before it registers the completed destination identity.
 * Release a registered token before preparing it again. */
/* Output tokens also permit capturing input from the same retained identity.
 * Input-only tokens never permit a write. Resource exhaustion returns ENOMEM. */
int lume_user_buffer_acquire(struct lume_user_buffer_token *, unsigned, uint64_t, size_t, bool);
bool lume_user_buffer_prepare(struct lume_user_buffer_token *, unsigned, uint64_t, size_t, bool);
bool lume_user_buffer_check(const struct lume_user_buffer_token *, size_t, size_t, bool);
bool lume_user_buffer_copy_at(struct lume_user_buffer_token *, size_t, void *, size_t, bool);
bool lume_user_buffer_copy(struct lume_user_buffer_token *, void *, size_t, bool);
void lume_user_buffer_release(struct lume_user_buffer_token *);
void lume_user_buffer_invalidate(unsigned, uint64_t, uint64_t);
void lume_user_buffer_invalidate_root(unsigned);
#endif
