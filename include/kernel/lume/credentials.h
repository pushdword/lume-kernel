#ifndef LUME_CREDENTIALS_H
#define LUME_CREDENTIALS_H
#include <stdint.h>

struct lume_files;
struct lume_identity;

int64_t lume_credential_call(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);
void lume_credential_exec(struct lume_identity *);
#endif
