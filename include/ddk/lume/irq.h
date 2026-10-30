#ifndef LUME_IRQ_H
#define LUME_IRQ_H
#include <stdbool.h>
#include <stdint.h>
#define LUME_IRQ_LIMIT 1020u
#define LUME_IRQ_DOMAIN_LIMIT 8u
#define LUME_IRQ_DOMAIN_LINES 64u

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

struct lume_irq_domain_ops
{
    int (*configure)(void *, unsigned irq, unsigned trigger);
    void (*enable)(void *, unsigned irq, bool);
    void (*acknowledge)(void *, unsigned irq);
};

/* Domain zero denotes the primary controller. Other IDs identify firmware hardware nodes.
 * Providers register with every line masked and retain their context until unregister succeeds.
 * Operations are copied. Configure and enable are required. Acknowledge is optional.
 * Dispatch accepts subordinate domains only. It acknowledges before invoking the handler.
 * The parent handler remains responsible for completing its own IRQ.
 * Requests for a missing provider return EAGAIN.
 * Mutating a domain during one of its callbacks returns EBUSY. Handlers cannot block. */
int lume_irq_domain_register(uint32_t id, unsigned lines, const struct lume_irq_domain_ops *,
                             void *context);
int lume_irq_domain_unregister(uint32_t id, void *context);
bool lume_irq_domain_available(uint32_t id);
int lume_irq_domain_request(uint32_t id, unsigned irq, unsigned trigger, lume_irq_handler,
                            void *context);
int lume_irq_domain_release(uint32_t id, unsigned irq, void *context);
int lume_irq_domain_enable(uint32_t id, unsigned irq, bool);
int lume_irq_domain_info(uint32_t id, unsigned irq, struct lume_irq_info *);
bool lume_irq_domain_dispatch(uint32_t id, unsigned irq);

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
