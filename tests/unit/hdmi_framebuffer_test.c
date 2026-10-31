#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/display/bcm2712_hdmi/firmware.c"

static unsigned leases, requests, blanks;
static bool fail_blank;
static bool mode_active;
static int map_result = -LUME_EBUSY, attach_result, detach_result;
static unsigned attaches, detaches;
static unsigned reported_depth = 32, reported_order, expected_order = 1;

int hdmi_hsm_acquire(bool *held)
{
    ++leases;
    *held = true;
    return 0;
}

int hdmi_hsm_release(void)
{
    assert(leases);
    --leases;
    return 0;
}

void lume_puts(const char *text)
{
    assert(text);
}

void lume_value(const char *name, uint64_t value)
{
    assert(name);
    (void)value;
}

int lume_pi5_firmware_property(uint32_t *message, size_t bytes)
{
    assert(message[0] == bytes);
    ++requests;
    switch (message[2])
    {
    case 0x00030001:
        message[4] = message[9] = UINT32_C(0x80000008);
        message[6] = 1;
        message[11] = 300000000;
        return 0;
    case 0x00040013:
        message[4] = UINT32_C(0x80000004);
        message[5] = 1;
        return 0;
    case 0x00030023:
        message[4] = UINT32_C(0x80000088);
        message[6] = 0;
        message[7] = UINT32_C(0xffffff00);
        message[8] = UINT32_C(0x00ffffff);
        return 0;
    case 0x00040016:
        message[4] = UINT32_C(0x80000004);
        message[5] = 2;
        return 0;
    case 0x00040017:
        message[4] = UINT32_C(0x80000024);
        message[6] = mode_active ? 148500 : 0;
        return 0;
    case 0x00048017:
        assert(message[5] == (2u | (16u << 16)) && message[6] == 148500);
        assert(message[7] == (1920u | (2008u << 16)));
        assert(message[11] == 1125 && message[12] == 60 && message[13] == 0x23);
        mode_active = true;
        message[4] = UINT32_C(0x80000004);
        message[5] = 0;
        return 0;
    case 0x00048019:
        assert(message[5] == 2 && message[6] == 1);
        message[4] = UINT32_C(0x80000008);
        return 0;
    case 0x00048013:
        message[4] = UINT32_C(0x80000004);
        return 0;
    case 0x00040003:
        message[4] = UINT32_C(0x80000008);
        message[5] = 1920;
        message[6] = 1080;
        message[9] = message[13] = UINT32_C(0x80000004);
        message[10] = reported_depth;
        message[14] = reported_order;
        return 0;
    case 0x00040001:
        message[4] = UINT32_C(0x80000008);
        message[5] = 0x3f800000;
        message[6] = 4147200;
        message[9] = UINT32_C(0x80000004);
        message[10] = 3840;
        return 0;
    case 0x00040002:
        ++blanks;
        message[4] = UINT32_C(0x80000004);
        return fail_blank ? -LUME_EIO : 0;
    case 0x00048001:
        message[4] = UINT32_C(0x80000000);
        return 0;
    default:
        assert(0);
        return -LUME_EINVAL;
    }
}

int lume_pi5_firmware_map_framebuffer(uint64_t physical, size_t bytes)
{
    assert(physical == 0x3f800000 && bytes == 4147200);
    return map_result;
}

bool lume_dma_sync_for_device(const void *memory, size_t bytes, enum lume_dma_direction direction)
{
    (void)memory;
    (void)bytes;
    (void)direction;
    assert(0);
    return false;
}

int hdmi_console_attach(uintptr_t memory, unsigned width, unsigned height, unsigned depth,
                        unsigned order, unsigned pitch, size_t bytes)
{
    assert(memory == 0x3f800000 && width == 1920 && height == 1080 && depth == 16);
    assert(order == expected_order && pitch == 3840 && bytes == 4147200);
    ++attaches;
    return attach_result;
}

int hdmi_console_detach(void)
{
    ++detaches;
    return detach_result;
}

int main(void)
{
    struct hdmi_framebuffer_context a = {0}, b = {0};
    assert(framebuffer_start(NULL, &a) == -LUME_EBUSY && a.allocated);
    unsigned before = requests;
    assert(framebuffer_start(NULL, &b) == -LUME_EBUSY);
    assert(!framebuffer_stop(NULL, &b) && requests == before && owner == &a && !leases);
    assert(!framebuffer_stop(NULL, &a) && !owner && !a.allocated && requests == before);
    assert(!framebuffer_stop(NULL, &a) && !blanks && !leases);
    map_result = 0;
    attach_result = -LUME_EBUSY;
    assert(framebuffer_start(NULL, &a) == -LUME_EBUSY && attaches == 1);
    assert(!a.console_registered && !framebuffer_stop(NULL, &a) && !detaches);
    attach_result = 0;
    assert(!framebuffer_start(NULL, &a) && a.allocated && a.console_registered);
    assert(hdmi_firmware_active() && owner == &a);
    detach_result = -LUME_EBUSY;
    before = requests;
    assert(framebuffer_stop(NULL, &a) == -LUME_EBUSY && a.console_registered && owner == &a);
    detach_result = 0;
    assert(!framebuffer_stop(NULL, &a) && !owner && !a.allocated && !a.console_registered);
    assert(!hdmi_firmware_active() && requests == before && !blanks && !leases);
    reported_depth = 16;
    assert(!framebuffer_start(NULL, &a) && a.console_registered);
    assert(!framebuffer_stop(NULL, &a) && !owner);
    reported_order = 1;
    expected_order = 0;
    assert(!framebuffer_start(NULL, &a) && a.console_registered);
    assert(!framebuffer_stop(NULL, &a) && !owner);
    puts("PASS HDMI framebuffer ownership and firmware scanout preservation");
}
