#ifndef LUME_NETIF_H
#define LUME_NETIF_H
#include "lume/abi.h"
#include "lume/net_abi.h"
#include <stdbool.h>
#include <stddef.h>

/* The kernel serializes access to this registry. Registrations keep stable indices until reboot.
 * Callback code and context must remain valid for that lifetime. Receive consumes one complete
 * Ethernet frame, returns zero when no frame is available and returns a negative value on error.
 * Transmit returns true when a frame is accepted and the caller may reuse its storage.
 * Driver callbacks must not enter the network stack or dispatch operations through this registry.
 * Poll callbacks may report asynchronous completion errors through the accounting helpers.
 */
struct lume_netif_ops
{
    void (*poll)(void *);
    bool (*link)(void *);
    int (*receive)(void *, void *, size_t);
    bool (*transmit)(void *, const void *, size_t);
    bool (*admin)(void *, bool);
};

struct lume_netif_descriptor
{
    const char *name;
    uint32_t kind, mtu;
    unsigned char mac[6];
    const struct lume_netif_ops *ops;
    void *context;
};
struct lume_netif;
int lume_netif_register(const struct lume_netif_descriptor *);
unsigned lume_netif_count(void);
struct lume_netif *lume_netif_at(unsigned);
struct lume_netif *lume_netif_find(uint32_t);
void lume_netif_info(const struct lume_netif *, struct lume_netif_info *);
int lume_netif_admin(struct lume_netif *, bool);
void lume_netif_poll(struct lume_netif *);
void lume_netif_sync(struct lume_netif *);
int lume_netif_receive(struct lume_netif *, void *, size_t);
bool lume_netif_transmit(struct lume_netif *, const void *, size_t);
/* Report errors discovered outside a transmit or receive dispatch. */
void lume_netif_tx_error(struct lume_netif *);
void lume_netif_rx_error(struct lume_netif *);
void lume_net_event_emit(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
int lume_net_events(uint32_t, uint64_t, struct lume_net_event *, unsigned, uint64_t *, uint32_t *);
#endif
