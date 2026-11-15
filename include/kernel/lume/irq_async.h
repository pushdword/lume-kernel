#ifndef LUME_IRQ_ASYNC_H
#define LUME_IRQ_ASYNC_H
#include "lume/irq.h"

/* Resident handlers acknowledge hardware and publish atomic events without
 * kernel services, allocation or suspension. Unregister returns EBUSY while a
 * handler retains its context. A failed unregister leaves the source masked. */
int lume_irq_register_async(unsigned, unsigned, lume_irq_handler, void *);
/* A false result leaves acknowledgement and EOI to the normal IRQ dispatcher. */
bool lume_irq_dispatch_async(unsigned token);
#endif
