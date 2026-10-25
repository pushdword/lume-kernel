#ifndef LUME_RESOURCE_H
#define LUME_RESOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUME_RESOURCE_CLAIM_LIMIT 96u
#define LUME_RESOURCE_LIMIT 16u

enum lume_resource_kind
{
    LUME_RESOURCE_MMIO = 1,
    LUME_RESOURCE_IRQ,
    LUME_RESOURCE_IOPORT
};

struct lume_resource
{
    uint64_t base, size;
    uint32_t kind, flags, domain, reserved;
};

/* Claim arrays remain immutable until release. Calls require serialized kernel context. */
int lume_resource_claim(const void *owner, const struct lume_resource *, size_t count);
int lume_resource_release(const void *owner);
bool lume_resource_owned(const void *owner);
unsigned lume_resource_claim_count(void);
#endif
