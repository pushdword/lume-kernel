#ifndef LUME_WATCHDOG_H
#define LUME_WATCHDOG_H
#include <stdbool.h>
#include <stdint.h>

#define LUME_PM_BASE UINT64_C(0x107d200000)
#define LUME_PM_RSTC 0x1cu
#define LUME_PM_RSTS 0x20u
#define LUME_PM_WDOG 0x24u
#define LUME_PM_PASSWORD UINT32_C(0x5a000000)
#define LUME_WATCHDOG_SECONDS 8u

struct lume_watchdog_io
{
    void *context;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
};

struct lume_watchdog_state
{
    bool active, runtime;
    uint64_t deadline, next_ping;
};

bool lume_watchdog_arm(const struct lume_watchdog_io *, unsigned seconds);
void lume_watchdog_disarm(const struct lume_watchdog_io *);
bool lume_watchdog_inherit(struct lume_watchdog_state *, const struct lume_watchdog_io *, uint64_t);
void lume_watchdog_step(struct lume_watchdog_state *, const struct lume_watchdog_io *, uint64_t);
void lume_watchdog_runtime(struct lume_watchdog_state *, uint64_t);
void lume_watchdog_call_begin(struct lume_watchdog_state *, uint64_t);
void lume_watchdog_call_end(struct lume_watchdog_state *, uint64_t);
void lume_watchdog_resume_user(struct lume_watchdog_state *, uint64_t, uint64_t spsr);

bool lume_watchdog_boot(void);
void lume_watchdog_poll(void);
void lume_watchdog_ready(void);
void lume_watchdog_enter_call(void);
void lume_watchdog_progress(void);
void lume_watchdog_user_progress(uint64_t spsr);
#endif
