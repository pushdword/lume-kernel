#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lume/platform_driver.h"
#include "lume/abi.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#define LUME_Q35_H
#define LUME_Q35_MODULE_H
#define Q35_MODULE(...)
struct lume_hardware;

struct q35_binding
{
    bool registered;
};

static unsigned reg_index, io_writes, vbe_version = 0xb0c5;
static uint16_t regs[16];
static unsigned cap_width = 1920, cap_height = 1200, cap_bytes = 16 * 1024 * 1024;
static bool pci_present = true, registered_display, registered_graphics, fail_graphics;
static bool virtio_present;
static unsigned pci_class = 0x0300;

int lume_q35_pci_find(uint16_t vendor, uint16_t product, uint32_t *address)
{
    *address = 0;
    if (vendor == 0x1234 && product == 0x1111)
        return pci_present ? 0 : -LUME_ENOENT;
    assert(vendor == 0x1af4 && product == 0x1050);
    return virtio_present ? 0 : -LUME_ENOENT;
}

void lume_out16(uint16_t port, uint16_t value)
{
    if (port == 0x1ce)
        reg_index = value;
    else
    {
        assert(port == 0x1cf && reg_index < 16);
        regs[reg_index] = value;
        ++io_writes;
    }
}

uint16_t lume_in16(uint16_t port)
{
    assert(port == 0x1cf && reg_index < 16);
    if (!reg_index)
        return (uint16_t)vbe_version;
    if (reg_index == 10)
        return cap_bytes / 65536;
    if (regs[4] & 2)
        return reg_index == 1 ? cap_width : reg_index == 2 ? cap_height : 32;
    return regs[reg_index];
}

uint32_t lume_q35_pci_read(uint32_t pci, unsigned offset)
{
    (void)pci;
    if (offset == 8)
        return pci_class << 16;
    assert(offset == 4 || offset == 16);
    return offset == 16 ? 0xe0000000u : 3;
}

void lume_q35_pci_write(uint32_t pci, unsigned offset, uint32_t value)
{
    (void)pci;
    assert(offset == 4 && value == 3);
}

void lume_fail(const char *message)
{
    fputs(message, stderr);
    abort();
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    (void)ops;
    if (kind == LUME_PLATFORM_DISPLAY)
    {
        assert(bytes == sizeof(struct lume_platform_display_ops) && !registered_display);
        registered_display = true;
    }
    else
    {
        assert(kind == LUME_PLATFORM_GRAPHICS &&
               bytes == sizeof(struct lume_platform_graphics_ops) && !registered_graphics);
        if (fail_graphics)
            return -LUME_EIO;
        registered_graphics = true;
    }
    return 0;
}

int lume_platform_driver_unregister(unsigned kind, const void *ops)
{
    (void)ops;
    if (kind == LUME_PLATFORM_DISPLAY)
    {
        assert(registered_display);
        registered_display = false;
    }
    else
    {
        assert(kind == LUME_PLATFORM_GRAPHICS && registered_graphics);
        registered_graphics = false;
    }
    return 0;
}

#include "../../drivers/display/bochs_vbe/display.c"

int main(void)
{
    unsigned allocated = cap_bytes + 4096;
    void *wanted = (void *)(uintptr_t)0xe0000000u;
#ifdef _WIN32
    void *memory = VirtualAlloc(wanted, allocated, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *memory = mmap(wanted, allocated, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    assert(memory == wanted);
    memset((char *)memory + cap_bytes, 0xa5, 4096);
#ifdef LUME_ARCH_I686
    pci_present = false;
    assert(lume_bochs_display_register() == -LUME_ENOENT && !registered_display);
    pci_present = true;
    fail_graphics = true;
    assert(lume_bochs_display_register() == -LUME_EIO && !registered_display &&
           !registered_graphics && !enabled && !framebuffer);
    fail_graphics = false;
    assert(!lume_bochs_display_register() && registered_display && registered_graphics);
#endif
    assert(start(0));
    assert(video_control(LUME_VIDEO_MODE_COUNT, 0, NULL) == 9);
    for (unsigned mode = 0; mode < 9; ++mode)
    {
        unsigned pixels = (unsigned)video_control(LUME_VIDEO_MODE_PIXELS, mode, NULL);
        assert(!video_control(LUME_VIDEO_MODE_SELECT, mode, NULL));
        assert(video_control(LUME_VIDEO_PIXELS, 0, NULL) == pixels);
        assert(regs[6] == (pixels & 65535) && regs[3] == 32);
        for (unsigned font = 0; font < CONSOLE_FONT_COUNT; ++font)
        {
            assert(!video_control(LUME_VIDEO_FONT_SELECT, font, NULL));
            assert(columns <= MAX_COLUMNS && rows <= MAX_ROWS);
            write("\033[31mA\xc3\xa7\xe2\x82\xac\033[0m",
                  sizeof("\033[31mA\xc3\xa7\xe2\x82\xac\033[0m") - 1);
            assert((cells[0] & 255) == 'A' && (cells[1] & 255) == 0xe7 && (cells[2] & 255) == 0x80);
        }
    }
    stop();
    vbe_version = 0xb0c2;
    assert(start(0));
    assert(video_control(LUME_VIDEO_MODE_COUNT, 0, NULL) == 3);
    stop();
    vbe_version = 0xb0c5;
    assert(start(0));
    unsigned before = io_writes;
    assert(video_control(LUME_VIDEO_MODE_SELECT, 9, NULL) == -LUME_EINVAL);
    assert(video_control(LUME_VIDEO_FONT_CELL, 8, NULL) == -LUME_EINVAL);
    assert(video_control(LUME_VIDEO_MODE_COUNT, 1, NULL) == -LUME_EINVAL);
    assert(before == io_writes);
    for (unsigned i = 0; i < 4096; ++i)
        assert(((unsigned char *)memory)[cap_bytes + i] == 0xa5);
    stop();
    cap_bytes = 4 * 1024 * 1024;
    cap_width = 1280;
    cap_height = 1024;
    assert(start(0));
    assert(video_control(LUME_VIDEO_MODE_COUNT, 0, NULL) == 4);
    unsigned char uploaded[4096];
    memset(uploaded, 0x33, sizeof(uploaded));
    assert(!video_control(LUME_VIDEO_UPLOAD, 0, uploaded));
    assert(video_control(LUME_VIDEO_FONT_COUNT, 0, NULL) == 9);
    assert(!memcmp(custom_font, uploaded, sizeof(uploaded)));
    uint32_t pixels[] = {0x123456, 0xabcdef, 0x112233, 0x445566};
    struct lume_graphics_request graphics = {.format = LUME_GRAPHICS_XRGB8888,
                                             .width = 2,
                                             .height = 2,
                                             .pitch = 8,
                                             .x = 11,
                                             .y = 23,
                                             .pixels = (uintptr_t)pixels};
    assert(!graphics_claim(true));
    assert(video_control(LUME_VIDEO_MODE_SELECT, 0, NULL) == -LUME_EBUSY);
    assert(!graphics_present(&graphics));
    assert(framebuffer[23 * width + 11] == pixels[0]);
    assert(framebuffer[24 * width + 12] == pixels[3]);
    write("console", 7);
    assert(framebuffer[23 * width + 11] == pixels[0]);
    graphics.x = width;
    assert(graphics_present(&graphics) == -LUME_EINVAL);
    assert(!graphics_claim(false));
    assert(graphics_present(&graphics) == -LUME_EBADF);
    stop();
#ifdef LUME_ARCH_I686
    assert(!lume_bochs_display_unregister());
    pci_present = false;
    virtio_present = true;
    pci_class = 0x0302;
    assert(lume_bochs_display_register() == -LUME_ENOENT);
    pci_class = 0x0300;
    assert(!lume_bochs_display_register());
    assert(!graphics_claim(true));
    assert(lume_bochs_display_unregister() == -LUME_EBUSY);
    assert(registered_display && registered_graphics);
    assert(!graphics_claim(false));
    assert(!lume_bochs_display_unregister());
    assert(!registered_display && !registered_graphics);
#endif
#ifdef _WIN32
    assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
    assert(!munmap(memory, allocated));
#endif
    puts("PASS Bochs catalogue, VRAM and size filtering, fonts, RGB32 bounds and invalid requests");
}
