#ifndef LUME_CREDENTIALS_H
#define LUME_CREDENTIALS_H
#include <stdbool.h>
#include <stdint.h>

struct lume_files;
struct lume_identity;
struct lume_inode_access;

int64_t lume_credential_call(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);
bool lume_credential_exec(struct lume_identity *, const struct lume_inode_access *);
#endif
