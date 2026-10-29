#include "lume/module_arch.h"
#include "lume/abi.h"

int lume_module_relocate_i686(unsigned type, void *target, size_t available, uint64_t place,
                              uint64_t symbol, int64_t addend)
{
    (void)type;
    (void)target;
    (void)available;
    (void)place;
    (void)symbol;
    (void)addend;
    return -LUME_ENOSYS;
}
