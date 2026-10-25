#ifndef LUME_IRQ_H
#define LUME_IRQ_H
#include <stdbool.h>
#include <stdint.h>
#define LUME_IRQ_LIMIT 1020u

enum
{
    LUME_IRQ_LEVEL = 0,
    LUME_IRQ_EDGE = 1
};

typedef void (*lume_irq_handler)(unsigned, void *);

struct lume_irq_controller
{
    int (*configure)(unsigned, unsigned);
    void (*enable)(unsigned, bool);
    void (*eoi)(unsigned);
    uint64_t (*save)(void);
    void (*restore)(uint64_t);
};

struct lume_irq_info
{
    uint64_t handled, stray;
    bool registered, enabled;
};

/* The controller serializes registration on one CPU. Handlers cannot block, allocate,
 * unregister themselves or enter subsystem callbacks. Device status must be acknowledged
 * before returning. The dispatcher completes the controller EOI. */
int lume_irq_controller_start(const struct lume_irq_controller *, unsigned lines);
int lume_irq_register(unsigned, unsigned trigger, lume_irq_handler, void *);
int lume_irq_enable(unsigned, bool);
int lume_irq_unregister(unsigned, void *context);
bool lume_irq_dispatch(unsigned token);
int lume_irq_info(unsigned, struct lume_irq_info *);
bool lume_irq_available(void);
uint64_t lume_irq_save(void);
void lume_irq_restore(uint64_t);
bool lume_pi5_irq_start(void);
void lume_pi5_irq_notify(void);
#endif
