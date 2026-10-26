#ifndef LUME_MODULE_SERVICES_H
#define LUME_MODULE_SERVICES_H
#include "lume/device.h"
#include "lume/devfs.h"
#include "lume/irq.h"
#include "lume/work.h"
#include "lume/netif.h"

int lume_module_device_attach(struct lume_device *);
int lume_module_device_detach(struct lume_device *);
int lume_module_register_block(struct lume_block *);
int lume_module_unregister_block(struct lume_block *);
int lume_module_irq_register(unsigned, unsigned, lume_irq_handler, void *);
int lume_module_irq_unregister(unsigned, void *);
int lume_module_work_submit(struct lume_work *);
bool lume_module_work_cancel(struct lume_work *);
int lume_module_netif_register(const struct lume_netif_descriptor *);
int lume_module_netif_unregister(uint32_t index);
#endif
