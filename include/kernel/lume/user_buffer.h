#ifndef LUME_USER_BUFFER_H
#define LUME_USER_BUFFER_H
#include "lume/abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_USER_BUFFER_PAGES ((LUME_FILE_IO_MAX + 4095u) / 4096u + 1u)
#define LUME_USER_RESIDENT_MAX 512u

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
/* Copy compact rows after one complete identity check under the scheduler lock. */
bool lume_user_buffer_copy_strided(struct lume_user_buffer_token *, void *, size_t, size_t,
                                   unsigned);
void lume_user_buffer_release(struct lume_user_buffer_token *);
void lume_user_buffer_invalidate(unsigned, uint64_t, uint64_t);
void lume_user_buffer_invalidate_root(unsigned);
/* Resident spans require the scheduler lock through their final copy. They do
 * not resolve faults or retain mappings across callbacks or suspension. */
bool lume_user_buffer_resident(unsigned, uint64_t, size_t, bool, uint64_t[2]);
bool lume_user_buffer_resident_remote(unsigned, uint64_t, size_t, bool, uint64_t[2]);
#endif
