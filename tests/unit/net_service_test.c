#include "lume/net.h"
#include "lume/work.h"
#include "lume/activity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t milliseconds;
static unsigned available, received_frames, checkpoints;
static bool active_stream, nested_notification;

uint64_t lume_clock_monotonic(void)
{
    return milliseconds;
}

uint32_t lume_clock_millis(void)
{
    return (uint32_t)milliseconds;
}

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t value)
{
    (void)value;
}

bool lume_stream_busy(void)
{
    return active_stream;
}

bool lume_stream_needs_poll(void)
{
    return active_stream;
}

bool lume_icmp_busy(void)
{
    return false;
}

bool lume_entropy(void *data, size_t bytes)
{
    memset(data, 1, bytes);
    return true;
}

void lume_puts(const char *text)
{
    (void)text;
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

unsigned lume_activity_enter(unsigned kind)
{
    (void)kind;
    return UINT32_MAX;
}

void lume_activity_leave(unsigned previous)
{
    (void)previous;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(2);
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes == 60);
    ++checkpoints;
    if (nested_notification)
    {
        nested_notification = false;
        unsigned before = received_frames;
        lume_net_poll_background();
        lume_net_poll();
        assert(received_frames == before);
    }
}

static void poll(void *context)
{
    (void)context;
}

static bool link(void *context)
{
    (void)context;
    return true;
}

static bool admin(void *context, bool up)
{
    (void)context;
    (void)up;
    return true;
}

static bool transmit(void *context, const void *data, size_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return true;
}

static int receive(void *context, void *data, size_t capacity)
{
    (void)context;
    assert(capacity >= 60);
    if (!available)
        return 0;
    --available;
    ++received_frames;
    memset(data, 0, 60);
    return 60;
}

static void idle_work(void *context)
{
    (void)context;
}

int main(void)
{
    lume_net_poll_background();
    assert(!lume_work_pending());
    const struct lume_netif_ops operations = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {.name = "eth0",
                                               .kind = LUME_IF_ETHERNET,
                                               .mtu = 1500,
                                               .mac = {2, 0, 0, 0, 0, 1},
                                               .ops = &operations};
    assert(lume_netif_register(&descriptor) == 1);
    struct lume_netif *interface = lume_netif_find(1);
    assert(!lume_netif_admin(interface, true));
    assert(lume_net_start());
    available = 20;
    nested_notification = true;
    for (unsigned i = 0; i < 5; ++i)
        lume_net_poll_background();
    assert(!received_frames && lume_work_pending());
    assert(lume_work_drain(1) == 1 && received_frames == 8 && lume_work_pending());
    assert(lume_work_drain(1) == 1 && received_frames == 16 && lume_work_pending());
    assert(lume_work_drain(1) == 1 && received_frames == 20 && !lume_work_pending());
    struct lume_net_statistics statistics;
    lume_netif_statistics(interface, &statistics);
    assert(statistics.rx_budget_exhausted == 2 && checkpoints == 20);
    milliseconds = 100;
    lume_net_poll_background();
    assert(lume_work_drain(1) == 1);
    milliseconds = 101;
    lume_net_poll_background();
    assert(!lume_work_pending());
    milliseconds = 110;
    lume_net_poll_background();
    assert(lume_work_drain(1) == 1);
    active_stream = true;
    milliseconds = 111;
    lume_net_poll_background();
    assert(lume_work_drain(1) == 1);
    active_stream = false;
    struct lume_work occupied[LUME_WORK_LIMIT];
    for (unsigned i = 0; i < LUME_WORK_LIMIT; ++i)
    {
        occupied[i] = (struct lume_work){.run = idle_work};
        assert(!lume_work_submit(occupied + i));
    }
    milliseconds = 121;
    lume_net_poll_background();
    assert(lume_work_drain(LUME_WORK_LIMIT) == LUME_WORK_LIMIT);
    lume_net_poll_background();
    assert(lume_work_drain(1) == 1 && !lume_work_pending());
    puts("PASS network deferred service, RX budget, rearming, serialization and queue pressure");
    return 0;
}
