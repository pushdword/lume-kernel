#ifndef LUME_IRQ_ASYNC_H
#define LUME_IRQ_ASYNC_H
#include "lume/irq.h"

/* A false result leaves acknowledgement and EOI to the normal IRQ dispatcher. */
bool lume_irq_dispatch_async(unsigned token);
void lume_irq_domain_poll(void);
#endif
