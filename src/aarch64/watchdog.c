#include "lume/watchdog.h"

bool lume_watchdog_arm(const struct lume_watchdog_io *io, unsigned seconds)
{
    if (!seconds || seconds > 15)
        return false;
    io->write(io->context, LUME_PM_WDOG, LUME_PM_PASSWORD | (seconds << 16));
    uint32_t control = io->read(io->context, LUME_PM_RSTC) & UINT32_C(0x00ffffcf);
    io->write(io->context, LUME_PM_RSTC, LUME_PM_PASSWORD | control | 0x20u);
    return true;
}

void lume_watchdog_disarm(const struct lume_watchdog_io *io)
{
    io->write(io->context, LUME_PM_RSTC, LUME_PM_PASSWORD | 0x102u);
}

bool lume_watchdog_inherit(struct lume_watchdog_state *s, const struct lume_watchdog_io *io,
                           uint64_t now)
{
    *s = (struct lume_watchdog_state){0};
    if (!(io->read(io->context, LUME_PM_RSTC) & 0x20u))
        return false;
    s->active = true;
    s->deadline = now + 90000;
    lume_watchdog_step(s, io, now);
    return true;
}

void lume_watchdog_step(struct lume_watchdog_state *s, const struct lume_watchdog_io *io,
                        uint64_t now)
{
    if (!s->active || now >= s->deadline || now < s->next_ping)
        return;
    (void)lume_watchdog_arm(io, LUME_WATCHDOG_SECONDS);
    s->next_ping = now + 1000;
}

void lume_watchdog_runtime(struct lume_watchdog_state *s, uint64_t now)
{
    if (s->active && now < s->deadline)
    {
        s->runtime = true;
        s->deadline = now + 5000;
    }
}

void lume_watchdog_call_begin(struct lume_watchdog_state *s, uint64_t now)
{
    /* A bounded syscall lease permits scans and DHCP without allowing a busy
     * kernel loop to keep the hardware watchdog alive indefinitely. */
    if (s->active && s->runtime && now < s->deadline)
        s->deadline = now + 45000;
}

void lume_watchdog_call_end(struct lume_watchdog_state *s, uint64_t now)
{
    if (s->active && s->runtime && now < s->deadline)
        s->deadline = now + 5000;
}

void lume_watchdog_resume_user(struct lume_watchdog_state *s, uint64_t now, uint64_t spsr)
{
    /* A completed scheduler tick from EL0 permits CPU-bound programs to run
     * without syscalls. Interrupts inside a stuck kernel are not progress. */
    if ((spsr & 15) == 0)
        lume_watchdog_call_end(s, now);
}
