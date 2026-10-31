//
// bcmframebuffer.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2020  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
/* The firmware framebuffer sequence is adapted to the Lume property interface.
 * The firmware chooses the HDMI mode. The CPU uses the existing framebuffer format. */
#include "lume/module.h"
#include "lume/abi.h"
#include "lume/console.h"
#include "lume/pi5_firmware.h"
#include "lume/dma.h"
#include "firmware.h"
#include "console.h"

static struct hdmi_framebuffer_context *owner;

bool hdmi_firmware_active(void)
{
    return owner && owner->allocated;
}

static void firmware_diagnostics(unsigned display)
{
    const unsigned ids[] = {4, 13, 14, 16};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
    {
        uint32_t clock[] = {52, 0, 0x00030001, 8, 4, ids[i], 0, 0x00030002, 8, 4, ids[i], 0, 0};
        int result = lume_pi5_firmware_property(clock, sizeof(clock));
        lume_value("HDMI_CLOCK_ID", ids[i]);
        if (result)
            lume_value("HDMI_CLOCK_ERROR", (uint64_t)(int64_t)result);
        else
        {
            lume_value("HDMI_CLOCK_STATE", clock[6]);
            lume_value("HDMI_CLOCK_HZ", clock[11]);
        }
    }
    uint32_t displays[] = {28, 0, 0x00040013, 4, 0, 0, 0};
    int result = lume_pi5_firmware_property(displays, sizeof(displays));
    if (!result)
        lume_value("HDMI_FIRMWARE_DISPLAYS", displays[5]);
    uint32_t edid[40] = {160, 0, 0x00030023, 136, 8, 0, display};
    result = lume_pi5_firmware_property(edid, sizeof(edid));
    if (result)
        lume_value("HDMI_EDID_ERROR", (uint64_t)(int64_t)result);
    else
    {
        lume_value("HDMI_EDID_RESPONSE", edid[4]);
        lume_value("HDMI_EDID_STATUS", edid[6]);
        lume_value("HDMI_EDID_HEADER", edid[7] | ((uint64_t)edid[8] << 32));
    }
}

static int framebuffer_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct hdmi_framebuffer_context *framebuffer = context;
    if (framebuffer && owner == framebuffer)
    {
        if (framebuffer->console_registered)
        {
            int result = hdmi_console_detach();
            if (result)
                return result;
            framebuffer->console_registered = false;
        }
        /* The firmware retains its framebuffer and scanout after detachment. */
        framebuffer->allocated = false;
        owner = NULL;
    }
    return 0;
}

static int framebuffer_start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct hdmi_framebuffer_context *framebuffer = context;
    if (!framebuffer)
        return -LUME_EINVAL;
    if (owner)
        return -LUME_EBUSY;
    owner = framebuffer;
    uint32_t select[] = {28, 0, 0x00048013, 4, 4, 0, 0};
    int result = lume_pi5_firmware_property(select, sizeof(select));
    if (result)
        return result;
    if (select[4] != UINT32_C(0x80000004))
        return -LUME_EIO;
    uint32_t display[] = {28, 0, 0x00040016, 4, 4, 0, 0};
    result = lume_pi5_firmware_property(display, sizeof(display));
    if (result || display[4] != UINT32_C(0x80000004))
        return result ? result : -LUME_EIO;
    lume_value("HDMI_FIRMWARE_DISPLAY_ID", display[5]);
    firmware_diagnostics(display[5]);
    uint32_t mode[] = {0, 0, 0x00040003, 8, 0, 0, 0, 0x00040005, 4, 0, 0, 0x00040006, 4, 0, 0, 0};
    mode[0] = sizeof(mode);
    result = lume_pi5_firmware_property(mode, sizeof(mode));
    if (result)
        return result;
    if (mode[4] != UINT32_C(0x80000008) || mode[9] != UINT32_C(0x80000004) ||
        mode[13] != UINT32_C(0x80000004))
        return -LUME_EIO;
    unsigned width = mode[5], height = mode[6], depth = mode[10], order = mode[14];
    uint32_t message[] = {0, 0, 0x00040001, 8, 4, 4096, 0, 0x00040008, 4, 0, 0, 0};
    message[0] = sizeof(message);
    result = lume_pi5_firmware_property(message, sizeof(message));
    if (result)
        return result;
    unsigned pitch = message[10];
    uint64_t physical = message[5] & UINT32_C(0x3fffffff);
    unsigned bytes = message[6];
    framebuffer->allocated = physical && bytes;
    /* Pi 5 firmware can echo a requested depth without changing the scanout format. */
    if (depth == 32 && width && pitch >= (uint64_t)width * 2 && pitch < (uint64_t)width * 4)
        depth = 16;
    /* RGB565 component order is opposite to the packed 32-bit color order. */
    if (depth == 16)
        order ^= 1;
    lume_value("HDMI_FB_PHYSICAL", physical);
    lume_value("HDMI_FB_BYTES", bytes);
    lume_value("HDMI_FB_WIDTH", width);
    lume_value("HDMI_FB_HEIGHT", height);
    lume_value("HDMI_FB_PITCH", pitch);
    lume_value("HDMI_FB_BPP", depth);
    lume_value("HDMI_FB_PIXEL_ORDER", order);
    if (message[4] != UINT32_C(0x80000008) || message[9] != UINT32_C(0x80000004) ||
        !framebuffer->allocated || (depth != 16 && depth != 32) || order > 1 || width < 64 ||
        width > 4096 || height < 64 || height > 2160 || (physical & 3u) || (pitch % (depth / 8)) ||
        pitch < width * (depth / 8) || bytes < (uint64_t)pitch * height)
        return -LUME_EIO;
    result = lume_pi5_firmware_map_framebuffer(physical, bytes);
    if (result)
        return result;
    result = hdmi_console_attach((uintptr_t)physical, width, height, depth, order, pitch, bytes);
    if (result)
        return result;
    framebuffer->console_registered = true;
    lume_puts("HDMI console ready\n");
    return 0;
}

const struct lume_hardware_ops hdmi_firmware_lifecycle = {
    .start = framebuffer_start,
    .stop = framebuffer_stop,
};
