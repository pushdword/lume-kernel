#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "lume/boot_trial.h"
#include "ff.h"
#include <string.h>

static FIL image;
static unsigned owner, phase, target, config_bytes, config_position;
static uint32_t total, offset, written_crc, read_crc;
/* A boot selection attempt blocks BEGIN until the kernel restarts. */
static bool selection_attempted;
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

static int read_configuration(const char *path, unsigned char out[512], unsigned *bytes,
                              unsigned *position)
{
    FIL file;
    UINT n = 0;
    if (f_open(&file, path, FA_READ) != FR_OK)
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

static int configuration(unsigned char out[512], unsigned *bytes, unsigned *position)
{
    return read_configuration("1:/config.txt", out, bytes, position);
}

static bool trial_volume(void)
{
    unsigned char text[512];
    unsigned bytes, position;
    return lume_boot_trial_supported() &&
           read_configuration("1:/tryboot.txt", text, &bytes, &position) >= 0;
}

static bool write_configuration(const char *path, const unsigned char *text, unsigned bytes)
{
    FIL file;
    UINT count = 0;
    if (f_open(&file, path, FA_WRITE) != FR_OK)
        return false;
    bool ok = f_size(&file) == bytes && f_write(&file, text, bytes, &count) == FR_OK &&
              count == bytes && f_sync(&file) == FR_OK;
    if (f_close(&file) != FR_OK)
        ok = false;
    unsigned char actual[512];
    unsigned size, position;
    return ok && read_configuration(path, actual, &size, &position) >= 0 && size == bytes &&
           !memcmp(actual, text, bytes);
}

static int confirm(struct lume_boot_request *r)
{
    if (!lume_boot_trial_running() || owner || selection_attempted)
        return -LUME_EBUSY;
    unsigned char stable[512], trial[512];
    unsigned bytes, position, trial_bytes, trial_position;
    int active = configuration(stable, &bytes, &position);
    int next = read_configuration("1:/tryboot.txt", trial, &trial_bytes, &trial_position);
    if (active < 0 || next < 0 || active == next || bytes != trial_bytes ||
        position != trial_position)
        return -LUME_EINVAL;
    memcpy(stable + position, slots[next] + 3, 12);
    if (memcmp(stable, trial, bytes))
        return -LUME_EINVAL;
    FIL file;
    if (f_open(&file, slots[next], FA_READ) != FR_OK)
        return -LUME_EIO;
    uint32_t size = f_size(&file), checksum = UINT32_MAX, read = 0;
    bool ok = size >= 1048576 && size <= 64 * 1024 * 1024 && !(size & 511);
    while (ok && read < size)
    {
        UINT count = size - read > sizeof(buffer) ? sizeof(buffer) : size - read, got = 0;
        ok = f_read(&file, buffer, count, &got) == FR_OK && got == count;
        if (ok)
        {
            checksum = crc(checksum, buffer, count);
            read += count;
        }
    }
    if (f_close(&file) != FR_OK)
        ok = false;
    if (!ok || !lume_boot_trial_matches(size, ~checksum))
        return -LUME_EIO;
    selection_attempted = true;
    if (!write_configuration("1:/config.txt", trial, bytes))
        return -LUME_EIO;
    lume_boot_trial_accept();
    r->active = (unsigned)next;
    r->total = size;
    r->crc = ~checksum;
    return 0;
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
    if (r->version != LUME_IO_VERSION || op > LUME_BOOT_CONFIRM || r->count > LUME_IO_BYTES)
        return -LUME_EINVAL;
    if (op == LUME_BOOT_CAPABILITIES)
    {
        r->total = LUME_BOOT_CAP_RESTART_GUARD;
        if (lume_storage_boot_mount() && trial_volume())
            r->total |= LUME_BOOT_CAP_TRIAL;
        return 0;
    }
    if (!lume_storage_boot_mount())
        return -LUME_EROFS;
    if (op == LUME_BOOT_CONFIRM)
        return confirm(r);
    if (op == LUME_BOOT_INFO || op == LUME_BOOT_BEGIN)
    {
        if ((owner || selection_attempted || lume_boot_trial_running()) && op == LUME_BOOT_BEGIN)
            return -LUME_EBUSY;
        unsigned char text[512];
        unsigned bytes, position;
        int active = configuration(text, &bytes, &position);
        if (op == LUME_BOOT_INFO && lume_boot_trial_running())
            active = read_configuration("1:/tryboot.txt", text, &bytes, &position);
        if (active < 0)
            return active;
        r->active = (unsigned)active;
        if (op == LUME_BOOT_INFO)
        {
            r->total = lume_boot_trial_running() ? LUME_BOOT_STATE_TRIAL : 0;
            return 0;
        }
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
    selection_attempted = true;
    bool trial = trial_volume();
    const char *path = trial ? "1:/tryboot.txt" : "1:/config.txt";
    if (!write_configuration(path, text, bytes) || (trial && !lume_boot_trial_arm()))
        return -LUME_EIO;
    r->active = target;
    r->crc = ~read_crc;
    lume_boot_release(pid);
    return 0;
}
