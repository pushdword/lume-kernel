#ifndef LUME_HDMI_CONSOLE_H
#define LUME_HDMI_CONSOLE_H
#include <stddef.h>
#include <stdint.h>

int hdmi_console_attach(uintptr_t, unsigned, unsigned, unsigned, unsigned, unsigned, size_t);
int hdmi_console_detach(void);
#endif
