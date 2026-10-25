#ifndef LUME_MODULE_MEMORY_H
#define LUME_MODULE_MEMORY_H
#include <stddef.h>

enum
{
    LUME_MODULE_READ = 1u,
    LUME_MODULE_WRITE = 2u,
    LUME_MODULE_EXECUTE = 4u
};

struct lume_module_memory
{
    void *address;
    size_t bytes;
};

/* Published code and data must be quiescent before their permissions change. */
int lume_module_memory_start(void);
int lume_module_memory_allocate(size_t bytes, struct lume_module_memory *);
int lume_module_memory_protect(const struct lume_module_memory *, size_t offset, size_t bytes,
                               unsigned flags);
int lume_module_memory_release(struct lume_module_memory *);
size_t lume_module_memory_available(void);
#endif
