#include "lume/q35.h"
#if defined(LUME_ARCH_I686)
#include "lume/hardware.h"
#include "lume/abi.h"

struct q35_binding
{
    bool registered;
};
#else
#include "lume/q35_module.h"
#endif
#include "lume/platform_driver.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/usb.h"
#include "lume/graphics_device.h"
#include <string.h>

static struct lume_hid keyboard;
static unsigned char report[8];
static bool extended, enabled;
static unsigned pause_bytes;
static bool mouse_enabled, input_graphical;
static unsigned mouse_bytes;
static unsigned char mouse_packet[3];
static const unsigned char usages[128] = {
    [1] = 41,  [2] = 30,  [3] = 31,  [4] = 32,  [5] = 33,  [6] = 34,  [7] = 35,  [8] = 36,
    [9] = 37,  [10] = 38, [11] = 39, [12] = 45, [13] = 46, [14] = 42, [15] = 43, [16] = 20,
    [17] = 26, [18] = 8,  [19] = 21, [20] = 23, [21] = 28, [22] = 24, [23] = 12, [24] = 18,
    [25] = 19, [26] = 47, [27] = 48, [28] = 40, [30] = 4,  [31] = 22, [32] = 7,  [33] = 9,
    [34] = 10, [35] = 11, [36] = 13, [37] = 14, [38] = 15, [39] = 51, [40] = 52, [41] = 53,
    [43] = 49, [44] = 29, [45] = 27, [46] = 6,  [47] = 25, [48] = 5,  [49] = 17, [50] = 16,
    [51] = 54, [52] = 55, [53] = 56, [55] = 85, [57] = 44, [58] = 57, [59] = 58, [60] = 59,
    [61] = 60, [62] = 61, [63] = 62, [64] = 63, [65] = 64, [66] = 65, [67] = 66, [68] = 67,
    [69] = 83, [70] = 71, [71] = 95, [72] = 96, [73] = 97, [74] = 86, [75] = 92, [76] = 93,
    [77] = 94, [78] = 87, [79] = 89, [80] = 90, [81] = 91, [82] = 98, [83] = 99, [86] = 100,
    [87] = 68, [88] = 69};

static bool controller_write(bool command, unsigned byte)
{
    uint64_t deadline = lume_counter_read() + lume_counter_frequency() / 20;
    while (lume_in8(0x64) & 2)
    {
        if (lume_counter_read() >= deadline)
            return false;
        lume_cpu_relax();
    }
    lume_out8(command ? 0x64 : 0x60, (unsigned char)byte);
    return true;
}

static bool mouse_command(unsigned command)
{
    if (!controller_write(true, 0xd4) || !controller_write(false, command))
        return false;
    uint64_t deadline = lume_counter_read() + lume_counter_frequency() / 20;
    while (lume_counter_read() < deadline)
    {
        unsigned status = lume_in8(0x64);
        if (status & 1)
        {
            unsigned value = lume_in8(0x60);
            if ((status & 0x20) && !(status & 0xc0))
                return value == 0xfa;
        }
        lume_cpu_relax();
    }
    return false;
}

static void mouse_byte(unsigned byte)
{
    if (!mouse_enabled || (!mouse_bytes && !(byte & 8)))
        return;
    mouse_packet[mouse_bytes++] = (unsigned char)byte;
    if (mouse_bytes != sizeof(mouse_packet))
        return;
    mouse_bytes = 0;
    if (mouse_packet[0] & 0xc0)
        return;
    int x = (int)mouse_packet[1] - (mouse_packet[0] & 0x10 ? 256 : 0);
    int y = (int)mouse_packet[2] - (mouse_packet[0] & 0x20 ? 256 : 0);
    struct lume_input_event event = {.type = LUME_INPUT_POINTER, .x = x, .y = -y,
                                      .buttons = mouse_packet[0] & 7,
                                      .milliseconds = lume_clock_millis()};
    lume_graphics_input(&event);
}

static bool keyboard_start(void)
{
    if (!controller_write(true, 0xad))
        return false;
    for (unsigned i = 0; i < 256 && (lume_in8(0x64) & 1); ++i)
        (void)lume_in8(0x60);
    /* Translation supplies set-1 scan codes. The keyboard retains its set-2 output. */
    if (!controller_write(true, 0x60) || !controller_write(false, 0x64) ||
        !controller_write(true, 0xae) || !controller_write(false, 0xf4))
        return false;
    mouse_bytes = 0;
    mouse_enabled = controller_write(true, 0xa8) && controller_write(true, 0x60) &&
                    controller_write(false, 0x44) && mouse_command(0xf6) && mouse_command(0xf4);
    if (!mouse_enabled)
    {
        (void)controller_write(true, 0xa7);
        (void)controller_write(true, 0x60);
        (void)controller_write(false, 0x64);
    }
    enabled = true;
    lume_puts("LUME PS2 KEYBOARD READY PT\n");
    return true;
}

static unsigned extended_usage(unsigned scan)
{
    switch (scan)
    {
    case 0x1c:
        return 88;
    case 0x35:
        return 84;
    case 0x47:
        return 74;
    case 0x48:
        return 82;
    case 0x49:
        return 75;
    case 0x4b:
        return 80;
    case 0x4d:
        return 79;
    case 0x4f:
        return 77;
    case 0x50:
        return 81;
    case 0x51:
        return 78;
    case 0x52:
        return 73;
    case 0x53:
        return 76;
    default:
        return 0;
    }
}

static void keyboard_poll(void)
{
    if (!enabled)
        return;
    bool graphical = lume_graphics_input_owned();
    if (graphical != input_graphical)
    {
        memset(&keyboard, 0, sizeof(keyboard));
        memset(report, 0, sizeof(report));
        input_graphical = graphical;
    }
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned status = lume_in8(0x64);
        if (!(status & 1))
            break;
        unsigned scan = lume_in8(0x60);
        if (status & 0xc0)
            continue;
        if (status & 0x20)
        {
            mouse_byte(scan);
            continue;
        }
        if (scan == 0xe0)
        {
            extended = true;
            continue;
        }
        if (scan == 0xe1)
        {
            pause_bytes = 5;
            continue;
        }
        if (pause_bytes)
        {
            --pause_bytes;
            continue;
        }
        bool release = (scan & 0x80) != 0;
        scan &= 0x7f;
        unsigned modifier = scan == 0x1d                ? (extended ? 16 : 1)
                            : scan == 0x38              ? (extended ? 64 : 4)
                            : !extended && scan == 0x2a ? 2
                            : !extended && scan == 0x36 ? 32
                                                        : 0;
        unsigned usage = extended ? extended_usage(scan) : usages[scan];
        if (graphical)
        {
            unsigned code = modifier == 1 ? 224 : modifier == 16 ? 228
                            : modifier == 4 ? 226 : modifier == 64 ? 230
                            : modifier == 2 ? 225 : modifier == 32 ? 229 : usage;
            if (code)
            {
                struct lume_input_event event = {.type = LUME_INPUT_KEY, .code = code,
                                                  .x = !release,
                                                  .milliseconds = lume_clock_millis()};
                lume_graphics_input(&event);
            }
            extended = false;
            continue;
        }
        extended = false;
        if (modifier)
            report[0] = release ? report[0] & ~modifier : report[0] | modifier;
        else if (usage)
        {
            if (release)
            {
                for (unsigned key = 2; key < 8; ++key)
                    if (report[key] == usage)
                        report[key] = 0;
            }
            else
            {
                unsigned available = 8;
                bool present = false;
                for (unsigned key = 2; key < 8; ++key)
                {
                    if (report[key] == usage)
                        present = true;
                    if (!report[key] && available == 8)
                        available = key;
                }
                if (!present && available < 8)
                    report[available] = (unsigned char)usage;
            }
        }
        lume_hid_report(&keyboard, report, sizeof(report), lume_clock_millis());
    }
    if (!graphical)
        lume_hid_repeat(&keyboard, lume_clock_millis());
}

static int keyboard_getc(void)
{
    keyboard_poll();
    if (lume_graphics_input_owned())
        return -LUME_EAGAIN;
    return lume_hid_getc(&keyboard);
}

static const struct lume_platform_keyboard_ops operations = {keyboard_poll, keyboard_getc};

static int module_start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    memset(&keyboard, 0, sizeof(keyboard));
    memset(report, 0, sizeof(report));
    extended = false;
    pause_bytes = 0;
    if (!keyboard_start())
        return -LUME_EIO;
    int result =
        lume_platform_driver_register(LUME_PLATFORM_KEYBOARD, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (!controller_write(true, 0xad))
        return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_KEYBOARD, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    enabled = false;
    memset(&keyboard, 0, sizeof(keyboard));
    memset(report, 0, sizeof(report));
    return 0;
}

#if defined(LUME_ARCH_I686)
int lume_pc_keyboard_start(void)
{
    static struct q35_binding binding;
    return module_start(NULL, &binding);
}
#else
Q35_MODULE("q35-ps2", LUME_HARDWARE_ID_BOARD, "qemu,ps2-keyboard", 1u << LUME_RESOURCE_IOPORT);
#endif
