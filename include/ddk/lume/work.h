#ifndef LUME_WORK_H
#define LUME_WORK_H
#include <stdbool.h>
#define LUME_WORK_LIMIT 32u

struct lume_work
{
    void (*run)(void *);
    void *context;
    bool queued;
};

/* Work storage must outlive its queued callback or a successful cancellation. Submit is
 * permitted from IRQ context. The kernel worker drains bounded batches outside IRQ handlers.
 * A callback queued during a suspended kernel operation waits for that operation to return. */
int lume_work_submit(struct lume_work *);
bool lume_work_cancel(struct lume_work *);
bool lume_work_pending(void);
unsigned lume_work_drain(unsigned budget);
#endif
