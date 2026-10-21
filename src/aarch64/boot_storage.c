#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "ff.h"
#include <string.h>

static FIL image;
static unsigned owner, phase, target, config_bytes, config_position;
static uint32_t total, offset, written_crc, read_crc;
static unsigned char config[512], buffer[LUME_IO_BYTES];
static const char *const slots[] = {"1:/lumeroot.fat", "1:/lumeslot.fat"};

static uint32_t crc(uint32_t value, const unsigned char *p, unsigned count)
{
    while (count--)
    {
        value ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            value = (value >> 1) ^ ((0u - (value & 1)) & UINT32_C(0xedb88320));
    }
    return value;
}

static int configuration(unsigned char out[512], unsigned *bytes, unsigned *position)
{
    FIL file;
    UINT n = 0;
    if (f_open(&file, "1:/config.txt", FA_READ) != FR_OK)
        return -LUME_EIO;
    bool ok = f_size(&file) < 512 && f_read(&file, out, 512, &n) == FR_OK;
    if (f_close(&file) != FR_OK || !ok)
        return -LUME_EIO;
    static const char prefix[] = "initramfs ";
    static const char suffix[] = " 0x08000000\n";
    int active = -LUME_EINVAL;
    for (unsigned i = 0; i + sizeof(prefix) - 1 + 12 + sizeof(suffix) - 1 <= n; ++i)
    {
        if ((i && out[i - 1] != '\n') || memcmp(out + i, prefix, sizeof(prefix) - 1))
            continue;
        unsigned p = i + sizeof(prefix) - 1;
        for (unsigned slot = 0; slot < 2; ++slot)
            if (!memcmp(out + p, slots[slot] + 3, 12) &&
                !memcmp(out + p + 12, suffix, sizeof(suffix) - 1))
            {
                if (active >= 0)
                    return -LUME_EINVAL;
                active = (int)slot;
                *position = p;
            }
    }
    *bytes = n;
    return active;
}

void lume_boot_release(unsigned pid)
{
    if (owner == pid)
    {
        if (phase == 1 || phase == 2)
            (void)f_close(&image);
        owner = phase = 0;
    }
}

int lume_boot_call(unsigned pid, unsigned op, struct lume_boot_request *r)
{
    if (r->version != LUME_IO_VERSION || op > LUME_BOOT_ABORT || r->count > LUME_IO_BYTES)
        return -LUME_EINVAL;
    if (!lume_storage_boot_mount())
        return -LUME_EROFS;
    if (op == LUME_BOOT_INFO || op == LUME_BOOT_BEGIN)
    {
        if (owner && op == LUME_BOOT_BEGIN)
            return -LUME_EBUSY;
        unsigned char text[512];
        unsigned bytes, position;
        int active = configuration(text, &bytes, &position);
        if (active < 0)
            return active;
        r->active = (unsigned)active;
        if (op == LUME_BOOT_INFO)
            return 0;
        if (r->total < 1024 * 1024 || r->total > 64 * 1024 * 1024 || (r->total & 511))
            return -LUME_EINVAL;
        target = 1u - (unsigned)active;
        if (f_open(&image, slots[target], FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
            return -LUME_EIO;
        memcpy(config, text, bytes);
        config_bytes = bytes;
        config_position = position;
        owner = pid;
        phase = 1;
        total = r->total;
        offset = 0;
        written_crc = read_crc = UINT32_MAX;
        r->offset = 0;
        return 0;
    }
    if (!owner || owner != pid)
        return -LUME_EPERM;
    if (op == LUME_BOOT_ABORT)
    {
        lume_boot_release(pid);
        return 0;
    }
    if (op == LUME_BOOT_WRITE)
    {
        if (phase != 1 || !r->count || r->offset != offset || r->count > total - offset)
            return -LUME_EINVAL;
        if (!offset && (r->count < 512 || r->data[510] != 0x55 || r->data[511] != 0xaa ||
                        r->data[11] != 0 || r->data[12] != 2))
            return -LUME_ENOEXEC;
        UINT n = 0;
        if (f_write(&image, r->data, r->count, &n) != FR_OK || n != r->count)
        {
            lume_boot_release(pid);
            return -LUME_EIO;
        }
        written_crc = crc(written_crc, r->data, n);
        offset += n;
        r->offset = offset;
        return (int)n;
    }
    if (op == LUME_BOOT_VERIFY)
    {
        if (phase == 1)
        {
            if (offset != total)
                return -LUME_EINVAL;
            FRESULT sync = f_sync(&image);
            FRESULT end = f_close(&image);
            phase = 3;
            if (sync != FR_OK || end != FR_OK || f_open(&image, slots[target], FA_READ) != FR_OK)
            {
                lume_boot_release(pid);
                return -LUME_EIO;
            }
            phase = 2;
            offset = 0;
            if (f_size(&image) != total)
            {
                lume_boot_release(pid);
                return -LUME_EIO;
            }
        }
        if (phase != 2)
            return -LUME_EINVAL;
        UINT n = 0;
        unsigned count = total - offset;
        if (count > sizeof(buffer))
            count = sizeof(buffer);
        if (f_read(&image, buffer, count, &n) != FR_OK || n != count)
        {
            lume_boot_release(pid);
            return -LUME_EIO;
        }
        read_crc = crc(read_crc, buffer, n);
        offset += n;
        r->offset = offset;
        r->total = total;
        r->crc = ~read_crc;
        if (offset < total)
            return 1;
        FRESULT end = f_close(&image);
        phase = 3;
        if (end != FR_OK || read_crc != written_crc)
        {
            lume_boot_release(pid);
            return -LUME_EIO;
        }
        phase = 4;
        return 0;
    }
    if (op != LUME_BOOT_ACTIVATE || phase != 4)
        return -LUME_EINVAL;
    unsigned char text[512];
    unsigned bytes, position;
    int active = configuration(text, &bytes, &position);
    if (active < 0 || (unsigned)active == target || bytes != config_bytes ||
        position != config_position || memcmp(config, text, bytes))
        return -LUME_EBUSY;
    memcpy(text + position, slots[target] + 3, 12);
    FIL file;
    UINT n = 0;
    if (f_open(&file, "1:/config.txt", FA_WRITE) != FR_OK)
        return -LUME_EIO;
    bool ok = f_write(&file, text, bytes, &n) == FR_OK && n == bytes && f_sync(&file) == FR_OK;
    if (f_close(&file) != FR_OK)
        ok = false;
    active = configuration(text, &bytes, &position);
    if (!ok || active != (int)target)
        return -LUME_EIO;
    r->active = target;
    r->crc = ~read_crc;
    lume_boot_release(pid);
    return 0;
}
