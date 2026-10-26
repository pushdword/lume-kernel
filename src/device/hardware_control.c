#include "lume/hardware.h"
#include "lume/abi.h"
#include <string.h>

int lume_hardware_control(struct lume_hardware_request *request, bool administer, bool diagnose)
{
    if (!request || request->version != LUME_HARDWARE_VERSION || request->reserved[0] ||
        request->reserved[1])
        return -LUME_EINVAL;
    switch (request->operation)
    {
    case LUME_HARDWARE_LIST:
        return lume_hardware_info(request->index, &request->info);
    case LUME_HARDWARE_GET:
        return lume_hardware_get(request->id, &request->info);
    case LUME_HARDWARE_BIND:
        if (!administer)
            return -LUME_EPERM;
        if (!memchr(request->driver, 0, sizeof(request->driver)))
            return -LUME_EINVAL;
        return lume_hardware_bind(request->id, lume_driver_find(request->driver));
    case LUME_HARDWARE_UNBIND:
        return administer ? lume_hardware_unbind(request->id) : -LUME_EPERM;
    case LUME_HARDWARE_RESCAN:
        return administer ? lume_hardware_rescan() : -LUME_EPERM;
    case LUME_HARDWARE_EJECT:
        return administer ? lume_hardware_eject(request->id) : -LUME_EPERM;
    case LUME_HARDWARE_READ_REGISTER:
        if (!diagnose)
            return -LUME_EPERM;
        return lume_hardware_read_register(request->id, request->resource, request->offset,
                                           request->width, &request->value);
    case LUME_HARDWARE_DRIVER:
    {
        const struct lume_driver *driver = lume_driver_at(request->index);
        if (!driver)
            return -LUME_ENOENT;
        memset(request->driver, 0, sizeof(request->driver));
        memcpy(request->driver, driver->name, strlen(driver->name));
        return 0;
    }
    default:
        return -LUME_EINVAL;
    }
}
