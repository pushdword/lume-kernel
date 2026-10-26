#include "lume/device.h"
#include "lume/abi.h"
#include "lume/early.h"
extern struct lume_device *const __lume_drivers_start[];
extern struct lume_device *const __lume_drivers_end[];

int lume_drivers_start(void)
{
    if ((size_t)(__lume_drivers_end - __lume_drivers_start) > LUME_DEVICE_LIMIT)
        return -LUME_ENOSPC;
    for (struct lume_device *const *p = __lume_drivers_start; p < __lume_drivers_end; ++p)
    {
        int result = lume_device_attach(*p);
        if (result)
        {
            lume_puts("LUME DRIVER FAILED ");
            lume_puts((*p)->name);
            lume_puts("\n");
            lume_value("DRIVER_ERROR", (uint64_t)(int64_t)result);
            while (p != __lume_drivers_start)
            {
                int cleanup = lume_device_detach(*--p);
                if (cleanup)
                    return cleanup;
            }
            return result;
        }
    }
    return 0;
}

int lume_drivers_stop(void)
{
    for (struct lume_device *const *p = __lume_drivers_end; p != __lume_drivers_start;)
    {
        int result = lume_device_detach(*--p);
        if (result)
            return result;
    }
    return 0;
}
