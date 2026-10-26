#ifndef LUME_BOOT_STORAGE_H
#define LUME_BOOT_STORAGE_H
#include "lume/io.h"
#include <stdbool.h>
bool lume_storage_boot_mount(void);
int lume_boot_call(unsigned, unsigned, struct lume_boot_request *);
void lume_boot_release(unsigned);
#endif
