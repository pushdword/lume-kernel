#ifndef LUME_MODULES_H
#define LUME_MODULES_H
#include "module_elf.h"
#include "lume/module_abi.h"

#define LUME_MODULE_LIMIT 16u
int lume_modules_start(const struct lume_module_linker *, const unsigned char build[20]);
int lume_module_load(const void *file, size_t bytes, struct lume_module_info *);
int lume_module_inspect(const void *file, size_t bytes, struct lume_module_info *);
int lume_module_unload(uint32_t id);
int lume_module_info(unsigned index, struct lume_module_info *);
void lume_module_kernel_info(struct lume_module_info *);
int lume_platform_modules_start(void);
void lume_platform_modules_boot(void);
int lume_module_pin(uintptr_t callback, void **owner);
void lume_module_unpin(void *owner);
#endif
