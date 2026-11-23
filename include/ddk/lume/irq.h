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
typedef bool (*lume_irq_shared_handler)(unsigned, void *);

struct lume_irq_action
{
    struct lume_irq_action *next;
    lume_irq_shared_handler handler;
    void *context;
};

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
/* Async providers use resident MMIO callbacks without kernel services. Defer
 * acknowledges a line before the scheduler lock and coalesces delivery to the
 * interrupt worker. Provider and handler storage remains retained until release. */
int lume_irq_domain_register_async(uint32_t id, unsigned lines, const struct lume_irq_domain_ops *,
                                   void *context);
bool lume_irq_domain_defer(uint32_t id, unsigned irq);
int lume_irq_domain_unregister(uint32_t id, void *context);
bool lume_irq_domain_available(uint32_t id);
int lume_irq_domain_request(uint32_t id, unsigned irq, unsigned trigger, lume_irq_handler,
                            void *context);
/* Deferred delivery runs outside the scheduler lock while retaining the domain
 * and handler. Shared state requires independent synchronization. Kernel metadata
 * services must bridge into the scheduler. Direct dispatch remains serialized.
 * Domain zero is not supported. Handlers must not suspend. */
int lume_irq_domain_request_async(uint32_t id, unsigned irq, unsigned trigger, lume_irq_handler,
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
/* Asynchronous handlers acknowledge resident MMIO and publish atomic events before
 * the scheduler lock. They cannot call kernel services. Unregister masks the line
 * and returns EBUSY until active handlers have released their retained context. */
int lume_irq_register_async(unsigned, unsigned trigger, lume_irq_handler, void *);
/* Shared level handlers run under the scheduler lock. Return true after acknowledging
 * the device. The first request unmasks the line and the last release masks it.
 * Keep the initially zeroed action alive until release returns successfully. */
int lume_irq_request_shared(unsigned, struct lume_irq_action *, lume_irq_shared_handler, void *);
int lume_irq_release_shared(unsigned, struct lume_irq_action *);
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
