#ifndef LUME_HDMI_FIRMWARE_H
#define LUME_HDMI_FIRMWARE_H
#include <stdbool.h>

struct hdmi_framebuffer_context
{
    bool allocated;
    bool console_registered;
};

int hdmi_hsm_acquire(bool *held);
int hdmi_hsm_release(void);
bool hdmi_firmware_active(void);
#endif
