#include "lume/irq.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static _Thread_local unsigned depth;
static unsigned root_configured, root_eoi, root_calls;
static bool root_enabled;

uint64_t lume_irq_save(void)
{
    return depth++;
}

void lume_irq_restore(uint64_t flags)
{
    assert(depth == flags + 1);
    depth = (unsigned)flags;
}

static int root_configure(unsigned irq, unsigned trigger)
{
    assert(irq == 42 && trigger == LUME_IRQ_LEVEL);
    ++root_configured;
    return 0;
}

static void root_enable(unsigned irq, bool enabled)
{
    assert(irq == 42);
    root_enabled = enabled;
}

static void root_complete(unsigned token)
{
    assert(token == 42);
    ++root_eoi;
}

static void root_handler(unsigned irq, void *context)
{
    assert(irq == 42 && context == &root_calls);
    ++root_calls;
}

struct backend
{
    uint32_t id;
    uint64_t enabled;
    unsigned configured, acknowledged, calls, last_irq;
    int configure_error;
    bool reenter;
};

static int configure(void *context, unsigned irq, unsigned trigger)
{
    struct backend *backend = context;
    assert(trigger == LUME_IRQ_EDGE);
    ++backend->configured;
    backend->last_irq = irq;
    if (backend->reenter)
    {
        assert(lume_irq_domain_unregister(backend->id, backend) == -LUME_EBUSY);
        assert(lume_irq_domain_enable(backend->id, irq, true) == -LUME_EBUSY);
        assert(!lume_irq_domain_dispatch(backend->id, irq));
    }
    return backend->configure_error;
}

static void enable(void *context, unsigned irq, bool enabled)
{
    struct backend *backend = context;
    uint64_t bit = UINT64_C(1) << irq;
    if (enabled)
        backend->enabled |= bit;
    else
        backend->enabled &= ~bit;
}

static void acknowledge(void *context, unsigned irq)
{
    struct backend *backend = context;
    ++backend->acknowledged;
    backend->last_irq = irq;
}

static void handler(unsigned irq, void *context)
{
    struct backend *backend = context;
    assert(backend->acknowledged > backend->calls && backend->last_irq == irq);
    ++backend->calls;
    if (backend->reenter)
    {
        struct lume_irq_info info;
        assert(!lume_irq_domain_info(backend->id, irq, &info));
        assert(info.handled == backend->calls);
        assert(lume_irq_domain_release(backend->id, irq, backend) == -LUME_EBUSY);
        assert(lume_irq_domain_unregister(backend->id, backend) == -LUME_EBUSY);
        assert(!lume_irq_domain_dispatch(backend->id, irq));
    }
}

static void unused_acknowledge(void *context, unsigned irq)
{
    (void)context;
    (void)irq;
    assert(false);
}

static unsigned dispatch_started, dispatch_finish;

static void blocking_handler(unsigned irq, void *context)
{
    assert(irq == 1 && context);
    __atomic_store_n(&dispatch_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&dispatch_finish, __ATOMIC_ACQUIRE))
    {
    }
}

#ifdef _WIN32
static DWORD WINAPI dispatch_thread(void *context)
#else
static void *dispatch_thread(void *context)
#endif
{
    struct backend *backend = context;
    assert(lume_irq_domain_dispatch(backend->id, 1));
    assert(!depth);
    return 0;
}

static void concurrent_dispatch(struct backend *backend, const struct lume_irq_domain_ops *ops)
{
    assert(!lume_irq_domain_register(backend->id, 32, ops, backend));
    assert(!lume_irq_domain_request(backend->id, 1, LUME_IRQ_EDGE, blocking_handler, backend));
    assert(!lume_irq_domain_enable(backend->id, 1, true));
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, dispatch_thread, backend, 0, NULL);
    assert(thread);
#else
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, dispatch_thread, backend));
#endif
    while (!__atomic_load_n(&dispatch_started, __ATOMIC_ACQUIRE))
    {
    }
    assert(lume_irq_domain_release(backend->id, 1, backend) == -LUME_EBUSY);
    assert(lume_irq_domain_unregister(backend->id, backend) == -LUME_EBUSY);
    assert(lume_irq_domain_enable(backend->id, 1, false) == -LUME_EBUSY);
    struct lume_irq_info info;
    assert(!lume_irq_domain_info(backend->id, 1, &info) && info.handled == 1);
    __atomic_store_n(&dispatch_finish, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread, NULL));
#endif
    assert(!lume_irq_domain_release(backend->id, 1, backend));
    assert(!lume_irq_domain_unregister(backend->id, backend));
}

int main(void)
{
    struct backend first = {.id = 27, .reenter = true}, second = {.id = 37};
    struct lume_irq_domain_ops ops = {configure, enable, acknowledge};
    assert(!lume_irq_domain_available(0));
    assert(lume_irq_domain_register(27, 32, &ops, &first) == -LUME_ENOSYS);
    const struct lume_irq_controller root = {root_configure, root_enable, root_complete,
                                             lume_irq_save, lume_irq_restore};
    assert(!lume_irq_controller_start(&root, 64));
    assert(lume_irq_domain_available(0));
    assert(lume_irq_domain_register(0, 32, &ops, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_register(27, 0, &ops, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_register(27, 65, &ops, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_register(27, 32, NULL, &first) == -LUME_EINVAL);
    struct lume_irq_domain_ops invalid = ops;
    invalid.enable = NULL;
    assert(lume_irq_domain_register(27, 32, &invalid, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, handler, &first) == -LUME_EAGAIN);
    assert(!lume_irq_domain_register(27, 32, &ops, &first));
    assert(!lume_irq_domain_register(37, 64, &ops, &second));
    assert(lume_irq_domain_available(27) && lume_irq_domain_available(37));
    assert(lume_irq_domain_register(27, 32, &ops, &first) == -LUME_EEXIST);
    assert(lume_irq_domain_unregister(27, &second) == -LUME_EPERM);
    ops.acknowledge = unused_acknowledge;

    struct lume_irq_info info = {0};
    assert(!lume_irq_domain_info(27, 1, &info) && !info.registered && !info.enabled);
    assert(lume_irq_domain_info(27, 32, &info) == -LUME_EINVAL);
    assert(lume_irq_domain_info(27, 1, NULL) == -LUME_EINVAL);
    assert(lume_irq_domain_enable(27, 1, true) == -LUME_ENOENT);
    first.configure_error = -LUME_EIO;
    assert(lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, handler, &first) == -LUME_EIO);
    assert(!lume_irq_domain_info(27, 1, &info) && !info.registered);
    first.configure_error = 0;
    assert(lume_irq_domain_request(27, 1, 2, handler, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, NULL, &first) == -LUME_EINVAL);
    assert(lume_irq_domain_request(27, 32, LUME_IRQ_EDGE, handler, &first) == -LUME_EINVAL);
    assert(!lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, handler, &first));
    assert(!lume_irq_domain_request(37, 1, LUME_IRQ_EDGE, handler, &second));
    assert(lume_irq_domain_request(27, 1, LUME_IRQ_EDGE, handler, &first) == -LUME_EBUSY);
    assert(lume_irq_domain_unregister(27, &first) == -LUME_EBUSY);
    assert(lume_irq_domain_release(27, 1, &second) == -LUME_EPERM);

    assert(!lume_irq_domain_enable(27, 1, true));
    assert(lume_irq_domain_dispatch(27, 1));
    assert(first.calls == 1 && second.calls == 0 && !root_eoi);
    assert(!lume_irq_domain_enable(37, 1, true));
    assert(lume_irq_domain_dispatch(37, 1));
    assert(first.calls == 1 && second.calls == 1 && !root_eoi);
    assert(!lume_irq_domain_enable(27, 1, false));
    assert(!lume_irq_domain_dispatch(27, 1));
    assert(!lume_irq_domain_info(27, 1, &info));
    assert(info.handled == 1 && info.stray == 1 && !info.enabled && info.registered);
    assert(!lume_irq_domain_dispatch(27, 2));
    assert(!lume_irq_domain_info(27, 2, &info) && info.stray == 1);
    assert(!lume_irq_domain_dispatch(27, 32));
    assert(!lume_irq_domain_dispatch(999, 1));
    assert(!lume_irq_domain_dispatch(0, 42));
    assert(!lume_irq_domain_release(27, 1, &first));
    assert(!lume_irq_domain_unregister(27, &first));
    assert(!lume_irq_domain_available(27));
    assert(lume_irq_domain_release(27, 1, &first) == -LUME_ENOENT);
    assert(lume_irq_domain_info(27, 1, &info) == -LUME_EAGAIN);
    assert(lume_irq_domain_unregister(27, &first) == -LUME_ENOENT);
    assert(lume_irq_domain_unregister(0, &first) == -LUME_EINVAL);
    assert(!lume_irq_domain_request(37, 63, LUME_IRQ_EDGE, handler, &second));
    assert(!lume_irq_domain_enable(37, 63, true));
    assert(second.enabled == (UINT64_C(1) << 63 | 2));
    assert(lume_irq_domain_dispatch(37, 63));
    assert(!lume_irq_domain_release(37, 63, &second));
    assert(!lume_irq_domain_release(37, 1, &second));
    assert(!lume_irq_domain_unregister(37, &second));

    assert(!lume_irq_domain_request(0, 42, LUME_IRQ_LEVEL, root_handler, &root_calls));
    assert(root_configured == 1);
    assert(!lume_irq_domain_enable(0, 42, true) && root_enabled);
    assert(lume_irq_dispatch(42) && root_eoi == 1 && root_calls == 1);
    assert(!lume_irq_domain_info(0, 42, &info) && info.handled == 1);
    assert(!lume_irq_domain_release(0, 42, &root_calls) && !root_enabled);

    ops.acknowledge = NULL;
    for (unsigned i = 0; i < LUME_IRQ_DOMAIN_LIMIT; ++i)
        assert(!lume_irq_domain_register(100 + i, 1, &ops, &first));
    assert(lume_irq_domain_register(200, 1, &ops, &first) == -LUME_ENOSPC);
    for (unsigned i = 0; i < LUME_IRQ_DOMAIN_LIMIT; ++i)
        assert(!lume_irq_domain_unregister(100 + i, &first));
    assert(!lume_irq_domain_register(27, 32, &ops, &first));
    assert(!lume_irq_domain_info(27, 1, &info));
    assert(!info.handled && !info.stray && !info.registered);
    assert(!lume_irq_domain_unregister(27, &first));
    first.reenter = false;
    concurrent_dispatch(&first, &ops);
    assert(!depth);
    puts("IRQ_DOMAIN_PASS isolation, lifecycle, concurrency, callbacks and primary compatibility");
    return 0;
}
