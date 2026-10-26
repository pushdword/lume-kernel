#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/usb.h"
#include <string.h>

static struct lume_hid keyboard;
static unsigned char report[8];
static bool extended;
static unsigned pause_bytes;
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

void lume_q35_keyboard_start(void)
{
    if (!controller_write(true, 0xad))
        return;
    for (unsigned i = 0; i < 256 && (lume_in8(0x64) & 1); ++i)
        (void)lume_in8(0x60);
    /* Translation supplies set-1 scan codes. The keyboard retains its set-2 output. */
    if (!controller_write(true, 0x60) || !controller_write(false, 0x64) ||
        !controller_write(true, 0xae) || !controller_write(false, 0xf4))
        return;
    lume_puts("LUME PS2 KEYBOARD READY PT\n");
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

void lume_q35_keyboard_poll(void)
{
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned status = lume_in8(0x64);
        if (!(status & 1))
            break;
        unsigned scan = lume_in8(0x60);
        if (status & 0xe0)
            continue;
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
    lume_hid_repeat(&keyboard, lume_clock_millis());
}

int lume_q35_keyboard_getc(void)
{
    lume_q35_keyboard_poll();
    return lume_hid_getc(&keyboard);
}
