/* SPDX-License-Identifier: GPL-2.0-only
 * RP1 register programming is adapted from the Raspberry Pi Linux clock, GPIO and PWM
 * drivers. Copyright (C) 2023 Raspberry Pi Ltd.
 * https://github.com/raspberrypi/linux/tree/rpi-6.18.y/drivers
 */
#include "lume/module.h"
#include "lume/abi.h"
#include "lume/module_build.h"
#include "lume/mmio.h"
#include "lume/console.h"
#include "lume/devfs.h"
#include <string.h>

/* /dev/cpufan/pct reports the clamped duty percentage and accepts one decimal command
 * per write. Zero is rejected. Initial attachment and successful detachment use 100%.
 * Clock and period values describe the programmed waveform, not a measured rotor speed.
 * RPM is exposed only when the firmware tree supplies the RP1 RPM mailbox register. */

#define PWM_REGISTER_BYTES UINT64_C(0x64)
#define PWM_GLOBAL_CTRL 0x00u
#define PWM_CHAN3_CTRL 0x44u
#define PWM_CHAN3_RANGE 0x48u
#define PWM_CHAN3_PHASE 0x4cu
#define PWM_CHAN3_DUTY 0x50u
#define PWM_GLOBAL_CHAN3_EN (UINT32_C(1) << 3)
#define PWM_GLOBAL_SET_UPDATE (UINT32_C(1) << 31)
#define PWM_CTRL_MODE_TRAILING UINT32_C(1)
#define PWM_CTRL_INVERT (UINT32_C(1) << 3)
#define PWM_POLL_LIMIT 100000u

#define RP1_CLK_PWM1 18u
#define RP1_CLOCK_PWM1 0x84u
#define CLOCK_CTRL 0x00u
#define CLOCK_DIV_INT 0x04u
#define CLOCK_DIV_FRAC 0x08u
#define CLOCK_ENABLE (UINT32_C(1) << 11)
#define CLOCK_AUX_MASK (UINT32_C(31) << 5)
#define CLOCK_XOSC (UINT32_C(2) << 5)
#define CLOCK_CONFIG_MASK (CLOCK_ENABLE | CLOCK_AUX_MASK)

/* GPIO45 is bank 2 pin 11. PWM1 channel 3 uses ALT0 on this pin. */
#define GPIO45_CTRL 0x805cu
#define GPIO45_PAD 0x8030u
#define GPIO_FUNCTION_MASK UINT32_C(0x1f)
#define GPIO_OUT_MASK (UINT32_C(3) << 12)
#define GPIO_OE_MASK (UINT32_C(3) << 14)
#define GPIO_FORCE_LOW ((UINT32_C(2) << 12) | GPIO_OE_MASK)
#define GPIO_CONFIG_MASK (GPIO_FUNCTION_MASK | GPIO_OUT_MASK | GPIO_OE_MASK)
#define PAD_PULL_MASK UINT32_C(0x0c)
#define PAD_PULL_DOWN UINT32_C(0x04)
#define PAD_INPUT_ENABLE UINT32_C(0x40)
#define PAD_OUTPUT_DISABLE UINT32_C(0x80)

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "raspberrypi,rp1-pwm"},
};

struct pwm_context
{
    struct lume_resource registers;
    /* Reserve only the PWM1 clock and the two GPIO45 registers in shared providers. */
    struct lume_resource auxiliary[3];
    struct lume_devfs_group group;
    uint32_t clock_hz, period_ns, range, duty_percent, min_percent, resource_index;
    uint32_t rpm_offset, divider;
    uint32_t saved_global, saved_control, saved_range, saved_phase, saved_duty;
    uint32_t saved_clock, saved_div_int, saved_div_frac, saved_gpio, saved_pad;
    bool rpm_available, claimed, saved, pwm_changed, started, registered;
};

static int setspeed(void *context, unsigned duty_percent);
static int enable(void *context, bool enabled);

#include "hardware.h"

static int probe(struct lume_hardware *hardware, void *context)
{
    if (!hardware || !context)
        return -LUME_EINVAL;
    struct pwm_context found = {0};
    bool have_registers = false;
    for (unsigned i = 0; i < LUME_HARDWARE_RESOURCES; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (have_registers)
            return -LUME_EINVAL;
        int result = valid_mmio(resource, PWM_REGISTER_BYTES);
        if (result)
            return result;
        found.registers = *resource;
        found.resource_index = i;
        have_registers = true;
    }
    if (!have_registers)
        return -LUME_EINVAL;
    int result = find_fan(lume_hardware_id(hardware), &found);
    if (!result)
        result = find_dependencies(hardware, &found);
    if (result)
        return result;
    uint64_t range = ((uint64_t)found.clock_hz * found.period_ns + 500000000u) / 1000000000u;
    if (!range || range > UINT32_MAX)
        return -LUME_EINVAL;
    found.range = (uint32_t)range;
    *(struct pwm_context *)context = found;
    return 0;
}

static int setspeed(void *context, unsigned duty_percent)
{
    struct pwm_context *pwm = context;
    if (!pwm || !pwm->registers.size || !pwm->started || !pwm->range || !duty_percent)
        return -LUME_EINVAL;

    if (duty_percent > 100)
    {
        lume_puts("RP1 PWM: CPU Fan tried to be set above 100%, limiting to 100%\n");
        duty_percent = 100;
    }
    if (duty_percent < pwm->min_percent)
    {
        lume_puts("RP1 PWM: CPU Fan tried to be set below minimum, limiting to minimum\n");
        lume_value("CPUFAN_MIN_PERCENT", pwm->min_percent);
        duty_percent = pwm->min_percent;
    }

    uintptr_t offset = (uintptr_t)pwm->registers.base + PWM_CHAN3_DUTY;
    /* Widen before multiplication and round up to keep a positive duty nonzero. */
    uint64_t duty = ((uint64_t)pwm->range * duty_percent + 99u) / 100u;
    lume_mmio_write32_sync(offset, (uint32_t)duty);
    if (lume_mmio_read32(offset) != (uint32_t)duty)
        return -LUME_EIO;
    pwm->duty_percent = duty_percent;
    /* DUTY has its own strobe and changes at the next counter overflow. */
    return 0;
}

static int enable(void *context, bool enabled)
{
    struct pwm_context *pwm = context;
    if (!pwm || !pwm->registers.size)
        return -LUME_EINVAL;
    uintptr_t offset = (uintptr_t)pwm->registers.base + PWM_GLOBAL_CTRL;
    uint32_t value = lume_mmio_read32(offset);
    bool present_value = (value & PWM_GLOBAL_CHAN3_EN) != 0;
    if (enabled != present_value)
    {
        /* These messages describe the programmed enable bit, not rotor movement. */
        if (enabled)
        {
            value |= PWM_GLOBAL_CHAN3_EN;
            lume_mmio_write32_sync(offset, value);
            lume_puts("RP1 PWM: CPU FAN enable on\n");
        }
        else
        {
            value &= ~PWM_GLOBAL_CHAN3_EN;
            lume_mmio_write32_sync(offset, value);
            lume_puts("RP1 PWM: CPU FAN enable off\n");
        }
    }
    /* Commit mode and phase even if the enable bit already has the requested value. */
    value |= PWM_GLOBAL_SET_UPDATE;
    (void)write_register(offset, value);
    lume_puts("RP1 PWM: CPU FAN enable set update requested\n");
    for (unsigned i = 0; i < PWM_POLL_LIMIT; ++i)
    {
        uint32_t observed = lume_mmio_read32(offset);
        if (!(observed & PWM_GLOBAL_SET_UPDATE))
            return (observed & PWM_GLOBAL_CHAN3_EN) == (value & PWM_GLOBAL_CHAN3_EN) ? 0
                                                                                     : -LUME_EIO;
    }
    return -LUME_EIO;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct pwm_context *pwm = context;
    if (!pwm)
        return -LUME_EINVAL;
    return fan_stop(pwm);
}

static int start(struct lume_hardware *hardware, void *context)
{
    struct pwm_context *pwm = context;
    if (!hardware || !pwm || valid_mmio(&pwm->registers, PWM_REGISTER_BYTES) || !pwm->range ||
        !pwm->clock_hz)
        return -LUME_EINVAL;

    /* Claim dependencies, save the original state and prepare the clock and GPIO45. */
    int result = fan_prepare(pwm);
    if (result)
        return result;

    uintptr_t offset_chan3_ctrl = (uintptr_t)pwm->registers.base + PWM_CHAN3_CTRL;
    uintptr_t offset_chan3_range = (uintptr_t)pwm->registers.base + PWM_CHAN3_RANGE;
    uintptr_t offset_chan3_phase = (uintptr_t)pwm->registers.base + PWM_CHAN3_PHASE;
    uintptr_t offset_chan3_duty = (uintptr_t)pwm->registers.base + PWM_CHAN3_DUTY;
    uint32_t ch3_ctrl_value = PWM_CTRL_MODE_TRAILING | PWM_CTRL_INVERT;
    uint32_t ch3_range_value = pwm->range;
    uint32_t ch3_phasevalue = 0;
    uint32_t ch3_duty_value = ch3_range_value;

    pwm->pwm_changed = true;
    result = enable(pwm, false);
    if (result)
        return result;
    /* Independent trailing-edge PWM starts with no phase shift and 100% duty. */
    if (write_register(offset_chan3_ctrl, ch3_ctrl_value) != ch3_ctrl_value ||
        write_register(offset_chan3_phase, ch3_phasevalue) != ch3_phasevalue ||
        write_register(offset_chan3_range, ch3_range_value) != ch3_range_value ||
        write_register(offset_chan3_duty, ch3_duty_value) != ch3_duty_value)
        return -LUME_EIO;
    result = enable(pwm, true);
    if (result)
        return result;
    return fan_publish(pwm);
}

static int read_register(struct lume_hardware *hardware, void *context, unsigned resource,
                         uint64_t offset, unsigned width, uint64_t *value)
{
    (void)hardware;
    struct pwm_context *pwm = context;
    if (!pwm || !value || !pwm->started || resource != pwm->resource_index || width != 4 ||
        (offset != PWM_GLOBAL_CTRL && offset != PWM_CHAN3_CTRL && offset != PWM_CHAN3_RANGE &&
         offset != PWM_CHAN3_PHASE && offset != PWM_CHAN3_DUTY &&
         (!pwm->rpm_available || offset != pwm->rpm_offset)))
        return -LUME_EACCES;
    *value = read_pwm(pwm, (unsigned)offset);
    return 0;
}

static const struct lume_hardware_ops lifecycle = {
    .probe = probe,
    .start = start,
    .stop = stop,
    .read_register = read_register,
};

static const struct lume_driver drivers[] = {
    {.version = LUME_DRIVER_VERSION,
     .name = "rp1-cpufan",
     .matches = matches,
     .match_count = sizeof(matches) / sizeof(matches[0]),
     .context_bytes = sizeof(struct pwm_context),
     .resource_kinds = 1u << LUME_RESOURCE_MMIO,
     .ops = &lifecycle},
};

const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("rp1-cpufan"),
    .drivers = drivers,
    .driver_count = sizeof(drivers) / sizeof(drivers[0]),
};
