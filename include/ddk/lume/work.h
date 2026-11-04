#ifndef LUME_WORK_H
#define LUME_WORK_H
#include <stdbool.h>
#include <stdint.h>
#define LUME_WORK_LIMIT 32u

enum lume_work_domain
{
    LUME_WORK_GENERAL,
    LUME_WORK_STORAGE,
    LUME_WORK_NETWORK,
    LUME_WORK_DISPLAY,
    LUME_WORK_DOMAINS
};

struct lume_work
{
    void (*run)(void *);
    void *context;
    bool queued;
    uint8_t domain;
};

/* Work storage must outlive its queued callback or a successful cancellation. Submit is
 * permitted from IRQ context. The kernel worker drains bounded batches outside IRQ handlers.
 * Callbacks in one resource domain execute in submission order. Independent domains can
 * progress while another domain waits for a device. General callbacks require exclusive entry.
 * The domain stays fixed until a running callback returns. Cancellation of a running callback
 * returns false and does not release its storage. */
int lume_work_submit(struct lume_work *);
bool lume_work_cancel(struct lume_work *);
bool lume_work_pending(void);
unsigned lume_work_drain(unsigned budget);
bool lume_work_pending_on(unsigned domain);
unsigned lume_work_drain_on(unsigned domain, unsigned budget);
#endif
