#ifndef LUME_PC_VIDEO_H
#define LUME_PC_VIDEO_H
#include <stdint.h>
int lume_s3_display_register(void);
unsigned lume_s3_console_geometry(void);
int64_t lume_s3_video_call(uint64_t action, uint64_t value, uint64_t reserved);
#endif
