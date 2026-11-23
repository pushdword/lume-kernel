/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_INTERRUPT_H
#define LUME_LINUX_INTERRUPT_H
#include <linux/kernel.h>
#include <lume/work.h>

typedef enum irqreturn
{
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_THREAD = 2
} irqreturn_t;

typedef irqreturn_t (*irq_handler_t)(int, void *);
struct device;

struct tasklet_struct
{
    struct tasklet_struct *next;
    void (*callback)(struct tasklet_struct *);
    void (*legacy_callback)(unsigned long);
    unsigned long data;
    volatile u32 state;
    volatile u32 disabled;
};

#define IRQF_SHARED 0x00000080UL
#define IRQF_ONESHOT 0x00002000UL

/* Shared primary handlers run in hard IRQ context. A handler may return IRQ_WAKE_THREAD to
 * schedule its thread function on a device worker. IRQF_ONESHOT is rejected until the host can
 * mask a shared line across every active thread. Mask the device before freeing its handler. */
int request_irq(unsigned int, irq_handler_t, unsigned long flags, const char *, void *);
int request_threaded_irq(unsigned int, irq_handler_t, irq_handler_t thread_fn, unsigned long flags,
                         const char *, void *);
const void *free_irq(unsigned int, void *);
int devm_request_threaded_irq(struct device *, unsigned int, irq_handler_t, irq_handler_t thread_fn,
                              unsigned long flags, const char *, void *);
void devm_free_irq(struct device *, unsigned int, void *);

static inline int devm_request_irq(struct device *owner, unsigned int irq, irq_handler_t handler,
                                   unsigned long flags, const char *name, void *device)
{
    return devm_request_threaded_irq(owner, irq, handler, NULL, flags, name, device);
}

void lume_linux_tasklet_setup(struct tasklet_struct *, void (*)(struct tasklet_struct *));
bool lume_linux_tasklet_busy(void);
void tasklet_init(struct tasklet_struct *, void (*)(unsigned long), unsigned long);
void tasklet_schedule(struct tasklet_struct *);
void tasklet_hi_schedule(struct tasklet_struct *);
void tasklet_disable_nosync(struct tasklet_struct *);
void tasklet_disable(struct tasklet_struct *);
void tasklet_enable(struct tasklet_struct *);
void tasklet_kill(struct tasklet_struct *);

#define tasklet_setup(tasklet, callback) lume_linux_tasklet_setup((tasklet), (callback))
#define from_tasklet(variable, callback_tasklet, member)                                        \
    container_of((callback_tasklet), typeof(*(variable)), member)
#define DECLARE_TASKLET(name, callback)                                                         \
    struct tasklet_struct name = {.callback = (callback)}
#endif
