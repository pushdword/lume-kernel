#include "lume/module.h"
#include "lume/vfs.h"
#include "lume/poll.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_MMIO_H
static uint32_t lume_mmio_read32(uintptr_t address);
static void lume_mmio_write32_sync(uintptr_t address, uint32_t value);
#include "../../drivers/pwm/rp1/cpufan.c"

static uintptr_t rp1;
static uint32_t pwm_regs[64], clock_regs[64], gpio_control, gpio_pad;
static unsigned writes, fail_write, updates;
static unsigned enable_on_messages, enable_off_messages, update_messages;
static bool stuck, no_rpm, no_levels;
static uint32_t parent_id, oscillator_id, clock_id, gpio_id, pwm_id, fan_id;
static uint32_t oscillator_hz = 50000000, requested_hz = 50000000, fan_channel = 3;
static uint32_t fan_period = 41566;

static uint32_t *reg(uintptr_t address)
{
    assert(!(address & 3u));
    if (address >= rp1 + 0x9c000 && address < rp1 + 0x9c100)
        return &pwm_regs[(address - rp1 - 0x9c000) / 4];
    if (address >= rp1 + 0x18000 && address < rp1 + 0x18100)
        return &clock_regs[(address - rp1 - 0x18000) / 4];
    if (address == rp1 + 0xd0000 + GPIO45_CTRL)
        return &gpio_control;
    assert(address == rp1 + 0xf0000 + GPIO45_PAD);
    return &gpio_pad;
}

static uint32_t lume_mmio_read32(uintptr_t address)
{
    uint32_t *value = reg(address);
    if (address == rp1 + 0x9c000 + PWM_GLOBAL_CTRL && !stuck)
        *value &= ~PWM_GLOBAL_SET_UPDATE;
    return *value;
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    assert(address != rp1 + 0x9c03c);
    ++writes;
    if (writes == fail_write)
        return;
    if (address == rp1 + 0x9c000 + PWM_GLOBAL_CTRL)
        updates += (value & PWM_GLOBAL_SET_UPDATE) != 0;
    *reg(address) = value;
}

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    assert(!bytes);
}

void lume_puts(const char *value)
{
    assert(value);
    enable_on_messages += !strcmp(value, "RP1 PWM: CPU FAN enable on\n");
    enable_off_messages += !strcmp(value, "RP1 PWM: CPU FAN enable off\n");
    update_messages += !strcmp(value, "RP1 PWM: CPU FAN enable set update requested\n");
}

void lume_value(const char *name, uint64_t value)
{
    assert(name && value);
}

static int firmware_property(const void *context, uint64_t token, const char *name, unsigned kind,
                             unsigned index, struct lume_property *out)
{
    (void)context;
    out->kind = kind;
    if (kind == LUME_PROPERTY_REFERENCE)
    {
        if (index)
            return -LUME_ENOENT;
        if (token == 1 && !strcmp(name, "clocks"))
            *out = (struct lume_property){.count = 2, .values = {clock_id, RP1_CLK_PWM1}};
        else if (token == 2 && !strcmp(name, "clocks"))
            *out = (struct lume_property){.count = 1, .values = {oscillator_id}};
        else if (token == 3 && !strcmp(name, "pwms"))
            *out =
                (struct lume_property){.count = 4, .values = {pwm_id, fan_channel, fan_period, 1}};
        else if (token == 3 && !strcmp(name, "rpm-regmap") && !no_rpm)
            *out = (struct lume_property){.count = 1, .values = {pwm_id}};
        else
            return -LUME_ENOENT;
        out->kind = kind;
        return 0;
    }
    if (kind != LUME_PROPERTY_U32)
        return -LUME_ENOENT;
    uint32_t value;
    if (token == 1 && !strcmp(name, "assigned-clock-rates") && !index)
        value = requested_hz;
    else if (token == 4 && !strcmp(name, "clock-frequency") && !index)
        value = oscillator_hz;
    else if (token == 3 && !strcmp(name, "rpm-offset") && !index)
        value = 0x3c;
    else if (token == 3 && !strcmp(name, "cooling-levels") && !no_levels)
    {
        static const uint32_t levels[] = {0, 75, 125, 175, 250};
        if (index >= sizeof(levels) / sizeof(levels[0]))
            return -LUME_EINVAL;
        value = levels[index];
    }
    else
        return -LUME_ENOENT;
    *out = (struct lume_property){.kind = kind, .count = 1, .values = {value}};
    return 0;
}

static const struct lume_firmware_ops firmware = {.property = firmware_property};

static uint32_t add_device(const char *path, const char *identity, unsigned parent,
                           const struct lume_resource *resources, size_t count, uint64_t token)
{
    struct lume_hardware_identity match = {.kind = LUME_HARDWARE_ID_DT};
    strcpy(match.value, identity);
    struct lume_hardware_description description = {.name = "fixture",
                                                    .path = path,
                                                    .origin = LUME_HARDWARE_DT,
                                                    .parent = parent,
                                                    .identities = &match,
                                                    .identity_count = 1,
                                                    .resources = resources,
                                                    .resource_count = count,
                                                    .firmware = &firmware,
                                                    .firmware_node = token};
    uint32_t id;
    assert(!lume_hardware_add(&description, &id));
    return id;
}

static void fixture(uintptr_t base)
{
    rp1 = base;
    memset(pwm_regs, 0, sizeof(pwm_regs));
    memset(clock_regs, 0, sizeof(clock_regs));
    pwm_regs[0] = PWM_GLOBAL_CHAN3_EN;
    pwm_regs[PWM_CHAN3_CTRL / 4] = 0x109;
    pwm_regs[PWM_CHAN3_RANGE / 4] = 1000;
    pwm_regs[PWM_CHAN3_PHASE / 4] = 7;
    pwm_regs[PWM_CHAN3_DUTY / 4] = 250;
    pwm_regs[0x3c / 4] = 4250;
    clock_regs[(RP1_CLOCK_PWM1 + CLOCK_CTRL) / 4] = CLOCK_ENABLE | (1u << 5);
    clock_regs[(RP1_CLOCK_PWM1 + CLOCK_DIV_INT) / 4] = 5;
    gpio_control = 0x01000005;
    gpio_pad = PAD_OUTPUT_DISABLE | PAD_INPUT_ENABLE;
    writes = updates = fail_write = 0;
    enable_on_messages = enable_off_messages = update_messages = 0;
    stuck = no_rpm = no_levels = false;
    oscillator_hz = requested_hz = 50000000;
    fan_channel = 3;
    fan_period = 41566;
    parent_id = add_device("/rp1", "simple-bus", 0, NULL, 0, 0);
    oscillator_id = add_device("/osc", "fixed-clock", 0, NULL, 0, 4);
    struct lume_resource clocks = {
        .base = rp1 + 0x18000, .size = 0x10038, .kind = LUME_RESOURCE_MMIO};
    clock_id = add_device("/rp1/clocks", "raspberrypi,rp1-clocks", parent_id, &clocks, 1, 2);
    struct lume_resource gpio[] = {
        {.base = rp1 + 0xd0000, .size = 0xc000, .kind = LUME_RESOURCE_MMIO},
        {.base = rp1 + 0xe0000, .size = 0xc000, .kind = LUME_RESOURCE_MMIO},
        {.base = rp1 + 0xf0000, .size = 0xc000, .kind = LUME_RESOURCE_MMIO}};
    gpio_id = add_device("/rp1/gpio", "raspberrypi,rp1-gpio", parent_id, gpio, 3, 0);
    struct lume_resource registers = {
        .base = rp1 + 0x9c000, .size = 0x100, .kind = LUME_RESOURCE_MMIO};
    pwm_id = add_device("/rp1/pwm1", "raspberrypi,rp1-pwm", parent_id, &registers, 1, 1);
    fan_id = add_device("/cooling_fan", "pwm-fan", 0, NULL, 0, 3);
}

static void clear(void)
{
    assert(!lume_hardware_unbind(pwm_id));
    assert(!lume_resource_claim_count());
    assert(!lume_hardware_remove(fan_id));
    assert(!lume_hardware_remove(pwm_id));
    assert(!lume_hardware_remove(gpio_id));
    assert(!lume_hardware_remove(clock_id));
    assert(!lume_hardware_remove(oscillator_id));
    assert(!lume_hardware_remove(parent_id));
}

static int validate(const struct lume_resource *resource)
{
    return valid_mmio(resource, 4);
}

static void read_text(struct lume_files *files, const char *path, const char *expected)
{
    char data[64] = {0};
    int fd = (int)lume_vfs_open(files, path, LUME_O_RDONLY);
    if (fd < 0 || lume_vfs_console_io(files, fd))
        fprintf(stderr, "Attribute open failed: %s fd=%d euid=%u\n", path, fd,
                files->identity.euid);
    assert(fd >= 0 && !lume_vfs_console_io(files, fd));
    assert(lume_vfs_read(files, fd, data, sizeof(data)) == (int64_t)strlen(expected));
    assert(!strcmp(data, expected));
    assert(!lume_vfs_read(files, fd, data, sizeof(data)));
    assert(!lume_vfs_close(files, fd));
}

static void normal(void)
{
    assert(!lume_hardware_bind(pwm_id, drivers));
    assert(lume_resource_claim_count() == 2);
    assert(pwm_regs[PWM_CHAN3_CTRL / 4] == 9);
    assert(pwm_regs[PWM_CHAN3_PHASE / 4] == 0);
    assert(pwm_regs[PWM_CHAN3_RANGE / 4] == 2078);
    assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 2078);
    assert(pwm_regs[0] == 8 && pwm_regs[0x3c / 4] == 4250 && updates == 2);
    assert(enable_on_messages == 1 && enable_off_messages == 1 && update_messages == 2);
    assert(!(gpio_control & GPIO_CONFIG_MASK));
    struct lume_files files;
    lume_files_init(&files);
    read_text(&files, "/dev/cpufan/pct", "100\n");
    read_text(&files, "/dev/cpufan/min_pct", "30\n");
    read_text(&files, "/dev/cpufan/rpm", "4250\n");
    read_text(&files, "/dev/cpufan/clock_hz", "50000000\n");
    read_text(&files, "/dev/cpufan/period_ns", "41560\n");
    int fd = (int)lume_vfs_open(&files, "/dev/cpufan/pct", LUME_O_RDWR | LUME_O_TRUNC);
    assert(fd >= 0);
    struct lume_stat stat;
    assert(lume_vfs_stat_path(&files, "/dev/cpufan/pct", &stat) == 0020644);
    assert(lume_vfs_stat_path(&files, "/dev/cpufan/rpm", &stat) == 0020444);
    char data[8] = {0};
    assert(lume_vfs_read(&files, fd, data, 1) == 1 && data[0] == '1');
    int other = (int)lume_vfs_open(&files, "/dev/cpufan/pct", LUME_O_WRONLY);
    assert(lume_vfs_write(&files, other, "50\n", 3) == 3);
    assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 1039 && updates == 2);
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == 3 && !memcmp(data, "00\n", 3));
    assert(!lume_vfs_seek(&files, fd, 0, 0));
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == 3 && !memcmp(data, "50\n", 3));
    static const char *invalid[] = {"0\n", "-1", "+20", "20x", "", " \n", "20\n30", "4294967296"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        unsigned before = writes;
        int64_t result = lume_vfs_write(&files, other, invalid[i], strlen(invalid[i]));
        assert(result == (invalid[i][0] ? -LUME_EINVAL : 0) && writes == before);
    }
    assert(lume_vfs_write(&files, other, "20\0", 3) == -LUME_EINVAL);
    assert(lume_vfs_write(&files, other, "1\n", 2) == 2);
    assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 624);
    read_text(&files, "/dev/cpufan/pct", "30\n");
    assert(lume_vfs_write(&files, other, " 33\r\n", 5) == 5);
    assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 686);
    unsigned before = writes;
    assert(lume_hardware_unbind(pwm_id) == -LUME_EBUSY && writes == before);
    assert(!lume_vfs_close(&files, other));
    assert(!lume_vfs_close(&files, fd));
    files.identity.euid = 1000;
    read_text(&files, "/dev/cpufan/pct", "33\n");
    assert(lume_vfs_open(&files, "/dev/cpufan/pct", LUME_O_WRONLY) == -LUME_EACCES);
    files.identity.euid = 0;
    assert(lume_vfs_open(&files, "/dev/cpufan/rpm", LUME_O_WRONLY) == -LUME_EROFS);
    int directory = (int)lume_vfs_open(&files, "/dev/cpufan", LUME_O_RDONLY);
    assert(directory >= 0 && lume_hardware_unbind(pwm_id) == -LUME_EBUSY);
    assert(!lume_vfs_close(&files, directory));
    assert(!lume_hardware_unbind(pwm_id));
    assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 2078 && pwm_regs[0] == 8);
    assert(lume_vfs_open(&files, "/dev/cpufan/pct", 0) == -LUME_ENOENT);
}

int main(void)
{
    static const struct lume_hardware_services services = {
        .allocate = malloc, .free = free, .validate_resource = validate};
    assert(!lume_hardware_services(&services));
    assert(!lume_driver_register(drivers));
    lume_vfs_init("", 0);
    for (unsigned i = 0; i < 2; ++i)
    {
        fixture(i ? UINT64_C(0x2100000000) : UINT64_C(0x1f00000000));
        normal();
        clear();
    }
    fixture(UINT64_C(0x1f00000000));
    oscillator_hz = 0;
    assert(lume_hardware_bind(pwm_id, drivers) == -LUME_EINVAL && !writes);
    oscillator_hz = 50000000;
    fan_channel = 0;
    assert(lume_hardware_bind(pwm_id, drivers) == -LUME_EINVAL && !writes);
    fan_channel = 3;
    pwm_regs[0] |= 1;
    assert(lume_hardware_bind(pwm_id, drivers) == -LUME_EBUSY && !writes);
    pwm_regs[0] &= ~1u;
    requested_hz = 40000000;
    assert(!lume_hardware_bind(pwm_id, drivers));
    assert(clock_regs[(RP1_CLOCK_PWM1 + CLOCK_DIV_FRAC) / 4] == 0x40000000);
    assert(pwm_regs[PWM_CHAN3_RANGE / 4] == 1663);
    clear();

    /* A single dropped write must either leave a valid configuration or roll back. */
    for (unsigned failure = 1; failure <= 15; ++failure)
    {
        fixture(UINT64_C(0x1f00000000));
        uint32_t original_pwm[64], original_clock[64];
        memcpy(original_pwm, pwm_regs, sizeof(pwm_regs));
        memcpy(original_clock, clock_regs, sizeof(clock_regs));
        uint32_t original_gpio = gpio_control, original_pad = gpio_pad;
        fail_write = failure;
        int result = lume_hardware_bind(pwm_id, drivers);
        if (result)
        {
            assert(!memcmp(original_pwm, pwm_regs, sizeof(pwm_regs)));
            assert(!memcmp(original_clock, clock_regs, sizeof(clock_regs)));
            assert(original_gpio == gpio_control && original_pad == gpio_pad);
        }
        else
            assert(pwm_regs[PWM_CHAN3_DUTY / 4] == 2078 && pwm_regs[0] == 8);
        clear();
    }
    fixture(UINT64_C(0x1f00000000));
    stuck = true;
    assert(lume_hardware_bind(pwm_id, drivers) == -LUME_EIO);
    assert(lume_resource_claim_count() == 2);
    stuck = false;
    clear();
    fixture(UINT64_C(0x1f00000000));
    no_rpm = no_levels = true;
    assert(!lume_hardware_bind(pwm_id, drivers));
    struct lume_files files;
    lume_files_init(&files);
    read_text(&files, "/dev/cpufan/min_pct", "100\n");
    assert(lume_vfs_open(&files, "/dev/cpufan/rpm", 0) == -LUME_ENOENT);
    clear();
    assert(!lume_driver_unregister(drivers));
    puts("RP1_CPUFAN_PASS discovery clocks GPIO duty devfs permissions snapshots rollback detach");
    return 0;
}
