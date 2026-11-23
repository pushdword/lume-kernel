/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_NETDEVICE_H
#define LUME_LINUX_NETDEVICE_H
#include <linux/skbuff.h>

#define LUME_LINUX_NET_RX_SLOTS 16u
#define LUME_LINUX_NAPI_SLOTS 8u
#define NAPI_POLL_WEIGHT 64

struct net_device;

struct napi_struct
{
    struct net_device *dev;
    int (*poll)(struct napi_struct *, int);
    int weight;
    volatile u32 enabled;
    volatile u32 scheduled;
    volatile u32 polling;
};

typedef enum
{
    GRO_NORMAL,
    GRO_DROP,
} gro_result_t;

typedef enum
{
    NETDEV_TX_OK = 0,
    NETDEV_TX_BUSY = 0x10
} netdev_tx_t;

enum
{
    NET_RX_SUCCESS = 0,
    NET_RX_DROP = 1
};

struct net_device_ops
{
    int (*ndo_open)(struct net_device *);
    int (*ndo_stop)(struct net_device *);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff *, struct net_device *);
};

struct net_device
{
    const struct net_device_ops *netdev_ops;
    const unsigned char *dev_addr;
    unsigned mtu;
    char name[16];
    /* Callbacks and packet queues use serialized task context. */
    void (*lume_poll)(struct net_device *);
    struct sk_buff *received[LUME_LINUX_NET_RX_SLOTS];
    struct napi_struct *napi[LUME_LINUX_NAPI_SLOTS];
    unsigned receive_head, receive_count;
    int ifindex;
    unsigned char address[6];
    bool running, carrier;
    void *lume_allocation;
};

void free_netdev(struct net_device *);
int register_netdev(struct net_device *);
int lume_linux_netdev_unregister(struct net_device *);
/* The receiver takes ownership even when the frame is dropped. */
int netif_rx(struct sk_buff *);
gro_result_t napi_gro_receive(struct napi_struct *, struct sk_buff *);
void netif_napi_add_weight(struct net_device *, struct napi_struct *,
                           int (*)(struct napi_struct *, int), int);
#define netif_napi_add(device, napi, callback)                                                   \
    netif_napi_add_weight((device), (napi), (callback), NAPI_POLL_WEIGHT)
void netif_napi_del(struct napi_struct *);
void napi_enable(struct napi_struct *);
void napi_disable(struct napi_struct *);
bool napi_schedule_prep(struct napi_struct *);
void __napi_schedule(struct napi_struct *);
void napi_schedule(struct napi_struct *);
bool napi_reschedule(struct napi_struct *);
bool napi_complete_done(struct napi_struct *, int);

static inline void *netdev_priv(const struct net_device *device)
{
    return (void *)((unsigned char *)device + ((sizeof(*device) + 63u) & ~(size_t)63u));
}

static inline void netif_carrier_on(struct net_device *device)
{
    device->carrier = true;
}

static inline void netif_carrier_off(struct net_device *device)
{
    device->carrier = false;
}
#endif
