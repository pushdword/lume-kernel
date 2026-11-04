#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_COUNTER_H
static uint32_t clock_ms;
static unsigned transport_depth, resets;
static bool transport_busy;

static uint64_t lume_counter_read(void)
{
    return (uint64_t)clock_ms * 1000;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

#include "../../drivers/net/cyw43455/firmware.c"

uint32_t lume_clock_millis(void)
{
    return clock_ms;
}

bool lume_wifi_claim(void)
{
    if (transport_busy)
        return false;
    ++transport_depth;
    return true;
}

void lume_wifi_release(void)
{
    assert(transport_depth);
    --transport_depth;
}

bool lume_worker_pause_ms(unsigned milliseconds)
{
    (void)milliseconds;
    return false;
}

void lume_wifi_stop(void)
{
    assert(transport_depth);
    ++resets;
}

void lume_secret_clear(void *data, size_t bytes)
{
    memset(data, 0, bytes);
}

struct lume_netif *lume_netif_find(uint32_t index)
{
    (void)index;
    abort();
}

void lume_netif_account(struct lume_netif *netif, unsigned event, uint64_t value)
{
    (void)netif;
    (void)event;
    (void)value;
    abort();
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

_Noreturn void lume_fail(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    abort();
}

static void be32(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[3 - i] = (unsigned char)(value >> (8 * i));
}

static void scan_event(unsigned status, uint16_t id, bool bss)
{
    memset(rx, 0, sizeof(rx));
    rx[0] = 0x20;
    unsigned char *event = rx + 4;
    event[12] = 0x88;
    event[13] = 0x6c;
    event[14] = 0x80;
    event[15] = 1;
    event[20] = 0x10;
    event[21] = 0x18;
    event[23] = 1;
    event[25] = 2;
    be32(event + 28, 69);
    be32(event + 32, status);
    unsigned bytes = status == 8 ? 12 + (bss ? 132 : 0) : 0;
    be32(event + 44, bytes);
    if (bytes)
    {
        unsigned char *payload = event + 72;
        put32(payload, bytes);
        payload[8] = (unsigned char)id;
        payload[9] = (unsigned char)(id >> 8);
        payload[10] = bss ? 1 : 0;
        if (bss)
        {
            unsigned char *entry = payload + 12;
            put32(entry, 109);
            put32(entry + 4, 132);
            entry[8] = 2;
            entry[13] = 7;
            entry[18] = 4;
            memcpy(entry + 19, "test", 4);
            entry[88] = 36;
            entry[116] = 132;
        }
    }
    struct lume_wifi_frame frame = {.channel = 1, .length = 4 + 72 + bytes, .offset = 0};
    dispatch(&frame);
}

static void reset(void)
{
    assert(!operation_depth && !transport_depth);
    operational = configured = scan_running = true;
    carrier = authentication_ready = data_in_flight = false;
    scan_done = scan_retiring = transport_busy = false;
    scan_id = 7;
    clock_ms = scan_started = scan_count = scan_errors = receive_count = receive_head = 0;
    resets = 0;
    memset(scan_results, 0, sizeof(scan_results));
}

static void cancelled_generation(void)
{
    reset();
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    assert(scan_running && scan_retiring && !scan_done && !operation_depth && !transport_depth);
    scan_event(8, 6, true);
    assert(!scan_count && !scan_errors && scan_running);
    scan_event(0, 7, false);
    assert(scan_done && !scan_running && !scan_retiring);
    assert(!cyw_scan_cancel(NULL) && !scan_done && !operation_depth && !transport_depth);
}

static void timeout_retains_generation(void)
{
    reset();
    struct lume_wifi_bss_info results[LUME_SCAN_MAX];
    clock_ms = 30000;
    assert(cyw_scan_result(NULL, results, LUME_SCAN_MAX) == -LUME_EIO);
    assert(scan_running && scan_retiring && !scan_done);
    clock_ms = 60000;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    assert(scan_running);
    scan_event(1, 7, false);
    assert(scan_errors == 1 && !scan_running);
    assert(!cyw_scan_cancel(NULL) && !operation_depth && !transport_depth);
}

static void results_and_reset(void)
{
    reset();
    struct lume_wifi_bss_info results[LUME_SCAN_MAX];
    assert(cyw_scan_result(NULL, results, LUME_SCAN_MAX) == -LUME_EAGAIN);
    scan_event(8, 7, true);
    assert(scan_count == 1 && !scan_errors);
    scan_event(8, 6, true);
    assert(scan_count == 1 && !scan_errors);
    scan_event(0, 7, false);
    assert(cyw_scan_result(NULL, results, LUME_SCAN_MAX) == 1);
    assert(!scan_running && results[0].channel == 36 && results[0].ssid_bytes == 4);
    assert(!memcmp(results[0].ssid, "test", 4));
    reset();
    transport_busy = true;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    assert(scan_running && !scan_retiring && !operation_depth && !transport_depth);
    transport_busy = false;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    lume_wifi_leave();
    assert(resets == 1 && !operational && !scan_running && !scan_done && !scan_retiring);
    assert(!cyw_scan_cancel(NULL) && !operation_depth && !transport_depth);
}

int main(void)
{
    cancelled_generation();
    timeout_retains_generation();
    results_and_reset();
    puts("PASS Wi-Fi scan retirement, terminal ownership, stale partials, timeout and reset");
    return 0;
}
