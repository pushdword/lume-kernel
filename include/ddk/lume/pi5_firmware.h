#ifndef LUME_PI5_FIRMWARE_H
#define LUME_PI5_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

/* Calls require serialized task context. The message includes its size, response code
 * and terminating zero tag. A timeout disables further calls until the next boot. */
int lume_pi5_firmware_property(uint32_t *message, size_t bytes);
/* Framebuffer storage must be excluded from allocatable RAM by the firmware tree. */
int lume_pi5_firmware_map_framebuffer(uint64_t physical, size_t bytes);
#endif
