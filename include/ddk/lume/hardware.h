#ifndef LUME_HARDWARE_H
#define LUME_HARDWARE_H
#include "lume/hardware_abi.h"
#include "resource.h"
#include "dma.h"

#define LUME_HARDWARE_LIMIT 256u
#define LUME_DRIVER_LIMIT 32u
#define LUME_DRIVER_MATCH_LIMIT 16u
#define LUME_DRIVER_VERSION 2u

enum lume_property_kind
{
    LUME_PROPERTY_U32 = 1,
    LUME_PROPERTY_U64,
    LUME_PROPERTY_STRING,
    LUME_PROPERTY_BOOLEAN,
    LUME_PROPERTY_REFERENCE,
    LUME_PROPERTY_U8
};

struct lume_property
{
    uint32_t kind, count;
    uint64_t values[8];
    char string[96];
};

struct lume_firmware_ops
{
    int (*property)(const void *context, uint64_t node, const char *name, unsigned kind,
                    unsigned index, struct lume_property *);
};

struct lume_hardware_description
{
    uint32_t parent, origin, state;
    int32_t error;
    const char *name, *path;
    const struct lume_hardware_identity *identities;
    size_t identity_count;
    const struct lume_resource *resources;
    size_t resource_count;
    const struct lume_firmware_ops *firmware;
    const void *firmware_context;
    uint64_t firmware_node;
};

struct lume_hardware;
struct lume_driver;

struct lume_hardware_ops
{
    int (*probe)(struct lume_hardware *, void *);
    int (*start)(struct lume_hardware *, void *);
    int (*stop)(struct lume_hardware *, void *);
    void (*release)(struct lume_hardware *, void *);
    int (*read_register)(struct lume_hardware *, void *, unsigned resource, uint64_t offset,
                         unsigned width, uint64_t *value);
};

struct lume_driver
{
    uint32_t version;
    const char *name;
    const struct lume_hardware_identity *matches;
    size_t match_count, context_bytes;
    /* Zero claims every resource kind. Polling drivers can select MMIO only. */
    uint32_t resource_kinds;
    const struct lume_hardware_ops *ops;
    void *owner;
    int (*owner_get)(void *);
    void (*owner_put)(void *);
};

struct lume_hardware_services
{
    void *(*allocate)(size_t);
    void (*free)(void *);
    int (*validate_resource)(const struct lume_resource *);
    int (*dma_domain)(uint32_t id, struct lume_dma_domain *);
};

struct lume_hardware_bus_ops
{
    int (*rescan)(void);
    int (*eject)(uint32_t id);
};

int lume_hardware_bus_services(const struct lume_hardware_bus_ops *);
int lume_hardware_rescan(void);
int lume_hardware_eject(uint32_t id);
int lume_hardware_state(uint32_t id, unsigned state, int error);

/* Descriptions are copied. Registered drivers and services outlive every association.
 * Lifecycle calls run with the kernel serialized. Stop handles partial initialization. */
int lume_hardware_services(const struct lume_hardware_services *);
int lume_hardware_add(const struct lume_hardware_description *, uint32_t *id);
int lume_hardware_remove(uint32_t id);
unsigned lume_hardware_count(void);
int lume_hardware_info(unsigned index, struct lume_hardware_info *);
int lume_hardware_get(uint32_t id, struct lume_hardware_info *);
uint32_t lume_hardware_id(const struct lume_hardware *);
int lume_hardware_dma(const struct lume_hardware *, struct lume_dma_domain *);
const struct lume_resource *lume_hardware_resource(const struct lume_hardware *, unsigned index);
uint32_t lume_hardware_firmware_id(const void *context, uint64_t node);
int lume_hardware_property(const struct lume_hardware *, const char *name, unsigned kind,
                           unsigned index, struct lume_property *);
int lume_hardware_property_id(uint32_t id, const char *name, unsigned kind, unsigned index,
                              struct lume_property *);
int lume_driver_register(const struct lume_driver *);
int lume_driver_unregister(const struct lume_driver *);
const struct lume_driver *lume_driver_find(const char *name);
unsigned lume_driver_count(void);
const struct lume_driver *lume_driver_at(unsigned index);
bool lume_driver_matches(const struct lume_driver *, uint32_t id);
int lume_hardware_bind(uint32_t id, const struct lume_driver *);
int lume_hardware_unbind(uint32_t id);
int lume_hardware_reference(uint32_t id);
int lume_hardware_unreference(uint32_t id);
int lume_hardware_read_register(uint32_t id, unsigned resource, uint64_t offset, unsigned width,
                                uint64_t *value);
int lume_hardware_control(struct lume_hardware_request *, bool administer, bool diagnose);
#endif
