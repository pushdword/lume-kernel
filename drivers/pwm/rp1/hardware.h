/* SPDX-License-Identifier: GPL-2.0-only
 * RP1 clock and GPIO programming follows the Raspberry Pi Linux drivers.
 * Copyright (C) 2023 Raspberry Pi Ltd.
 */
#ifndef LUME_RP1_CPUFAN_HARDWARE_H
#define LUME_RP1_CPUFAN_HARDWARE_H

/* Register and context declarations are supplied by the channel driver. */
static bool compatible(const struct lume_hardware_info *info, const char *value)
{
    for (unsigned i = 0; i < info->identity_count; ++i)
        if (info->identities[i].kind == LUME_HARDWARE_ID_DT &&
            !strcmp(info->identities[i].value, value))
            return true;
    return false;
}

static int valid_mmio(const struct lume_resource *resource, uint64_t bytes)
{
    if (!resource || resource->kind != LUME_RESOURCE_MMIO || resource->reserved ||
        resource->flags || resource->domain || resource->size < bytes || (resource->base & 3u) ||
        resource->base > UINT64_MAX - resource->size)
        return -LUME_EINVAL;
    return resource->base > UINTPTR_MAX - (bytes - 1) ? -LUME_ENOSYS : 0;
}

static int slice(const struct lume_hardware_info *info, unsigned index, uint32_t offset,
                 uint32_t bytes, struct lume_resource *out)
{
    for (unsigned i = 0; i < info->resource_count; ++i)
    {
        const struct lume_hardware_resource_info *source = &info->resources[i];
        if (source->kind != LUME_RESOURCE_MMIO)
            continue;
        if (index)
        {
            --index;
            continue;
        }
        struct lume_resource resource = {.base = source->base,
                                         .size = source->size,
                                         .kind = source->kind,
                                         .flags = source->flags,
                                         .domain = source->domain,
                                         .reserved = source->reserved};
        int result = valid_mmio(&resource, (uint64_t)offset + bytes);
        if (result)
            return result;
        resource.base += offset;
        resource.size = bytes;
        *out = resource;
        return 0;
    }
    return -LUME_ENOENT;
}

static int property_u32(uint32_t id, const char *name, unsigned index, uint32_t *value)
{
    struct lume_property property = {0};
    int result = lume_hardware_property_id(id, name, LUME_PROPERTY_U32, index, &property);
    if (!result)
    {
        if (property.count != 1 || property.values[0] > UINT32_MAX)
            return -LUME_EINVAL;
        *value = (uint32_t)property.values[0];
    }
    return result;
}

static int find_fan(uint32_t pwm_id, struct pwm_context *pwm)
{
    struct lume_hardware_info info;
    bool found = false;
    for (unsigned i = 0; !lume_hardware_info(i, &info); ++i)
    {
        if (!compatible(&info, "pwm-fan"))
            continue;
        struct lume_property property = {0};
        int result =
            lume_hardware_property_id(info.id, "pwms", LUME_PROPERTY_REFERENCE, 0, &property);
        if (result)
            return result;
        if (property.count != 4 || property.values[0] != pwm_id)
            continue;
        if (found)
            return -LUME_EEXIST;
        /* The Pi 5 connector uses channel 3 with an electrically inverted signal. */
        if (property.values[1] != 3 || property.values[3] != 1 || property.values[2] < 40000 ||
            property.values[2] > 50000)
            return -LUME_EINVAL;
        pwm->period_ns = (uint32_t)property.values[2];
        found = true;

        /* The lowest nonzero cooling level supplies the manual duty floor.
         * A missing table permits full speed only. No rotor motion is inferred. */
        pwm->min_percent = 100;
        unsigned previous = 0;
        bool positive = false;
        for (unsigned j = 0; j <= 16; ++j)
        {
            uint32_t level;
            result = property_u32(info.id, "cooling-levels", j, &level);
            if (result == -LUME_ENOENT && !j)
                break;
            if (result == -LUME_EINVAL && j)
                break;
            if (result)
                return result;
            if (j == 16 || level > 255 || (j && level <= previous))
                return -LUME_EINVAL;
            if (level && !positive)
            {
                pwm->min_percent = (level * 100u + 254u) / 255u;
                positive = true;
            }
            previous = level;
        }

        result =
            lume_hardware_property_id(info.id, "rpm-regmap", LUME_PROPERTY_REFERENCE, 0, &property);
        if (result != -LUME_ENOENT)
        {
            if (result)
                return result;
            if (property.count != 1 || property.values[0] != pwm_id)
                return -LUME_EINVAL;
            result = property_u32(info.id, "rpm-offset", 0, &pwm->rpm_offset);
            if (result || pwm->rpm_offset != 0x3c)
                return result ? result : -LUME_EINVAL;
            /* RP1 firmware publishes RPM in CHAN2_PHASE. Never write this register. */
            pwm->rpm_available = true;
        }
    }
    return found ? 0 : -LUME_ENOENT;
}

static int find_dependencies(struct lume_hardware *hardware, struct pwm_context *pwm)
{
    struct lume_property reference = {0};
    int result = lume_hardware_property(hardware, "clocks", LUME_PROPERTY_REFERENCE, 0, &reference);
    if (result)
        return result;
    if (reference.count != 2 || reference.values[0] > UINT32_MAX ||
        reference.values[1] != RP1_CLK_PWM1)
        return -LUME_EINVAL;
    struct lume_hardware_info clocks, controller, oscillator;
    result = lume_hardware_get((uint32_t)reference.values[0], &clocks);
    if (result)
        return result;
    result = lume_hardware_get(lume_hardware_id(hardware), &controller);
    if (result)
        return result;
    if (!compatible(&clocks, "raspberrypi,rp1-clocks") || !clocks.parent ||
        clocks.parent != controller.parent)
        return -LUME_EINVAL;
    result = slice(&clocks, 0, RP1_CLOCK_PWM1, 16, &pwm->auxiliary[0]);
    if (result)
        return result;
    result = lume_hardware_property_id(clocks.id, "clocks", LUME_PROPERTY_REFERENCE, 0, &reference);
    if (result)
        return result;
    if (reference.count != 1 || reference.values[0] > UINT32_MAX)
        return -LUME_ENOSYS;
    result = lume_hardware_get((uint32_t)reference.values[0], &oscillator);
    if (result)
        return result;
    if (!compatible(&oscillator, "fixed-clock"))
        return -LUME_ENOSYS;
    uint32_t xosc_hz;
    result = property_u32(oscillator.id, "clock-frequency", 0, &xosc_hz);
    if (result)
        return result;
    if (xosc_hz < 1000000 || xosc_hz > 76800000)
        return -LUME_EINVAL;
    uint32_t requested_hz;
    result = property_u32(controller.id, "assigned-clock-rates", 0, &requested_hz);
    if (result == -LUME_ENOENT)
        requested_hz = xosc_hz;
    else if (result)
        return result;
    if (!requested_hz || requested_hz > xosc_hz)
        return -LUME_ENOSYS;
    /* Use the fixed oscillator without changing a PLL shared with other devices.
     * The divider contains 16 integer bits followed by 16 fractional bits. */
    uint64_t divider = (((uint64_t)xosc_hz << 16) + requested_hz / 2u) / requested_hz;
    if (divider < UINT64_C(0x10000) || divider > UINT32_C(0xffff0000))
        return -LUME_EINVAL;
    pwm->divider = (uint32_t)divider;
    pwm->clock_hz = (uint32_t)(((uint64_t)xosc_hz << 16) / divider);
    bool found = false;
    struct lume_hardware_info gpio;
    for (unsigned i = 0; !lume_hardware_info(i, &gpio); ++i)
        if (gpio.parent == controller.parent && compatible(&gpio, "raspberrypi,rp1-gpio"))
        {
            if (found)
                return -LUME_EEXIST;
            result = slice(&gpio, 0, GPIO45_CTRL, 4, &pwm->auxiliary[1]);
            if (!result)
                result = slice(&gpio, 2, GPIO45_PAD, 4, &pwm->auxiliary[2]);
            if (result)
                return result;
            found = true;
        }
    return found ? 0 : -LUME_ENOENT;
}

static uint32_t read_pwm(const struct pwm_context *pwm, unsigned offset)
{
    return lume_mmio_read32((uintptr_t)pwm->registers.base + offset);
}

static uint32_t write_register(uintptr_t address, uint32_t value)
{
    lume_mmio_write32_sync(address, value);
    /* A read from RP1 drains posted PCIe writes before checking device state. */
    return lume_mmio_read32(address);
}

static int write_clock_control(struct pwm_context *pwm, uint32_t value)
{
    uintptr_t address = (uintptr_t)pwm->auxiliary[0].base + CLOCK_CTRL;
    uint32_t current = lume_mmio_read32(address);
    uint32_t requested = (current & ~CLOCK_CONFIG_MASK) | (value & CLOCK_CONFIG_MASK);
    uint32_t observed = write_register(address, requested);
    /* Clock status can change independently of the enable and source controls. */
    if ((observed ^ requested) & CLOCK_CONFIG_MASK)
    {
        lume_puts("RP1 PWM: clock configuration did not latch\n");
        lume_value("CPUFAN_REGISTER", address);
        lume_value("CPUFAN_EXPECTED", requested);
        lume_value("CPUFAN_OBSERVED", observed);
        return -LUME_EIO;
    }
    return 0;
}

static int write_pwm(struct pwm_context *pwm, unsigned offset, uint32_t value)
{
    return write_register((uintptr_t)pwm->registers.base + offset, value) == value ? 0 : -LUME_EIO;
}

static int number(uint32_t value, char *data, size_t capacity)
{
    char reverse[10];
    unsigned count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    if (capacity < count + 1u)
        return -LUME_E2BIG;
    for (unsigned i = 0; i < count; ++i)
        data[i] = reverse[count - i - 1];
    data[count] = '\n';
    return (int)count + 1;
}

static int read_percent(void *context, char *data, size_t capacity)
{
    return number(((struct pwm_context *)context)->duty_percent, data, capacity);
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int write_percent(void *context, const char *data, size_t bytes)
{
    size_t first = 0;
    while (first < bytes && space(data[first]))
        ++first;
    while (bytes > first && space(data[bytes - 1]))
        --bytes;
    if (first == bytes)
        return -LUME_EINVAL;
    uint32_t value = 0;
    for (size_t i = first; i < bytes; ++i)
    {
        if (data[i] < '0' || data[i] > '9' ||
            value > (UINT32_MAX - (unsigned)(data[i] - '0')) / 10u)
            return -LUME_EINVAL;
        value = value * 10u + (unsigned)(data[i] - '0');
    }
    return setspeed(context, value);
}

static int read_clock(void *context, char *data, size_t capacity)
{
    return number(((struct pwm_context *)context)->clock_hz, data, capacity);
}

static int read_period(void *context, char *data, size_t capacity)
{
    struct pwm_context *pwm = context;
    uint32_t actual =
        (uint32_t)(((uint64_t)pwm->range * 1000000000u + pwm->clock_hz / 2u) / pwm->clock_hz);
    return number(actual, data, capacity);
}

static int read_minimum(void *context, char *data, size_t capacity)
{
    return number(((struct pwm_context *)context)->min_percent, data, capacity);
}

static int read_rpm(void *context, char *data, size_t capacity)
{
    struct pwm_context *pwm = context;
    return number(read_pwm(pwm, pwm->rpm_offset), data, capacity);
}

static const struct lume_devfs_attribute attributes[] = {
    {.name = "pct", .read = read_percent, .write = write_percent},
    {.name = "clock_hz", .read = read_clock},
    {.name = "period_ns", .read = read_period},
    {.name = "min_pct", .read = read_minimum},
    {.name = "rpm", .read = read_rpm},
};

static int restore(struct pwm_context *pwm)
{
    if (!pwm->saved)
        return 0;
    if (pwm->pwm_changed)
    {
        int result = enable(pwm, false);
        if (!result)
            result = write_pwm(pwm, PWM_CHAN3_CTRL, pwm->saved_control);
        if (!result)
            result = write_pwm(pwm, PWM_CHAN3_PHASE, pwm->saved_phase);
        if (!result)
            result = write_pwm(pwm, PWM_CHAN3_RANGE, pwm->saved_range);
        if (!result)
            result = write_pwm(pwm, PWM_CHAN3_DUTY, pwm->saved_duty);
        if (!result)
            result = enable(pwm, (pwm->saved_global & PWM_GLOBAL_CHAN3_EN) != 0);
        if (result)
            return result;
    }
    uintptr_t clock = (uintptr_t)pwm->auxiliary[0].base;
    int result = write_clock_control(pwm, pwm->saved_clock & ~CLOCK_ENABLE);
    if (result)
        return result;
    if (write_register(clock + CLOCK_DIV_INT, pwm->saved_div_int) != pwm->saved_div_int ||
        write_register(clock + CLOCK_DIV_FRAC, pwm->saved_div_frac) != pwm->saved_div_frac)
        return -LUME_EIO;
    result = write_clock_control(pwm, pwm->saved_clock);
    if (result)
        return result;
    if (write_register((uintptr_t)pwm->auxiliary[2].base, pwm->saved_pad) != pwm->saved_pad ||
        write_register((uintptr_t)pwm->auxiliary[1].base, pwm->saved_gpio) != pwm->saved_gpio)
        return -LUME_EIO;
    pwm->saved = false;
    pwm->pwm_changed = false;
    return 0;
}

static int fan_stop(struct pwm_context *pwm)
{
    if (!pwm)
        return -LUME_EINVAL;
    /* Preflight leaves open descriptions and hardware unchanged on EBUSY. */
    if (pwm->registered)
    {
        int result = lume_devfs_can_unregister_group(&pwm->group);
        if (result)
            return result;
    }
    if (pwm->started)
    {
        /* A successful detach leaves autonomous full-speed cooling in hardware. */
        int result = setspeed(pwm, 100);
        if (result)
            return result;
    }
    else
    {
        int result = restore(pwm);
        if (result)
            return result;
    }
    if (pwm->registered)
    {
        int result = lume_devfs_unregister_group(&pwm->group);
        if (result)
            return result;
        pwm->registered = false;
    }
    pwm->started = false;
    if (pwm->claimed)
    {
        int result = lume_resource_release(pwm);
        if (result)
            return result;
        pwm->claimed = false;
    }
    return 0;
}

static int fan_prepare(struct pwm_context *pwm)
{
    if (pwm->claimed || pwm->started)
        return -LUME_EBUSY;
    int result = lume_resource_claim(pwm, pwm->auxiliary, 3);
    if (result)
        return result;
    pwm->claimed = true;
    pwm->saved_global = read_pwm(pwm, PWM_GLOBAL_CTRL);
    /* PWM1 shares its clock across four channels. Do not retime another active output. */
    if (pwm->saved_global & 7u)
        return -LUME_EBUSY;
    pwm->saved_control = read_pwm(pwm, PWM_CHAN3_CTRL);
    pwm->saved_phase = read_pwm(pwm, PWM_CHAN3_PHASE);
    pwm->saved_range = read_pwm(pwm, PWM_CHAN3_RANGE);
    pwm->saved_duty = read_pwm(pwm, PWM_CHAN3_DUTY);
    uintptr_t clock = (uintptr_t)pwm->auxiliary[0].base;
    uintptr_t gpio = (uintptr_t)pwm->auxiliary[1].base;
    uintptr_t pad = (uintptr_t)pwm->auxiliary[2].base;
    pwm->saved_clock = lume_mmio_read32(clock + CLOCK_CTRL);
    pwm->saved_div_int = lume_mmio_read32(clock + CLOCK_DIV_INT);
    pwm->saved_div_frac = lume_mmio_read32(clock + CLOCK_DIV_FRAC);
    pwm->saved_gpio = lume_mmio_read32(gpio);
    pwm->saved_pad = lume_mmio_read32(pad);
    pwm->saved = true;

    /* Driving the inverted fan input low requests full speed during reconfiguration. */
    uint32_t gpio_safe = (pwm->saved_gpio & ~GPIO_CONFIG_MASK) | GPIO_FORCE_LOW;
    uint32_t pad_value =
        (pwm->saved_pad & ~(PAD_OUTPUT_DISABLE | PAD_PULL_MASK)) | PAD_INPUT_ENABLE | PAD_PULL_DOWN;
    if (write_register(gpio, gpio_safe) != gpio_safe || write_register(pad, pad_value) != pad_value)
        return -LUME_EIO;
    result = write_clock_control(pwm, pwm->saved_clock & ~CLOCK_ENABLE);
    if (result)
        return result;
    uint32_t clock_value =
        (pwm->saved_clock & ~(CLOCK_ENABLE | CLOCK_AUX_MASK)) | CLOCK_XOSC | CLOCK_ENABLE;
    if (write_register(clock + CLOCK_DIV_INT, pwm->divider >> 16) != pwm->divider >> 16 ||
        write_register(clock + CLOCK_DIV_FRAC, pwm->divider << 16) != pwm->divider << 16)
        return -LUME_EIO;
    return write_clock_control(pwm, clock_value);
}

static int fan_publish(struct pwm_context *pwm)
{
    uintptr_t gpio = (uintptr_t)pwm->auxiliary[1].base;
    uint32_t gpio_pwm = pwm->saved_gpio & ~GPIO_CONFIG_MASK;
    if (write_register(gpio, gpio_pwm) != gpio_pwm)
        return -LUME_EIO;
    pwm->duty_percent = 100;
    pwm->group = (struct lume_devfs_group){.name = "cpufan",
                                           .attributes = attributes,
                                           .count = pwm->rpm_available ? 5 : 4,
                                           .context = pwm};
    int result = lume_devfs_register_group(&pwm->group);
    if (result)
        return result;
    pwm->registered = true;
    pwm->started = true;
    lume_value("CPUFAN_CLOCK_HZ", pwm->clock_hz);
    lume_value("CPUFAN_RANGE", pwm->range);
    lume_puts("RP1 PWM: CPU fan control ready at 100%\n");
    return 0;
}
#endif
