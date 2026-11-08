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
static bool hardware_active, stop_fails;
static const struct lume_sdio_bus test_bus;
static bool bus_fails, write_fails, wpa_invalid;
static unsigned register_reads, fifo_reads, fifo_writes, wpa_inputs, rsn_inputs;
static unsigned other_interface_polls, timer_passes;
static unsigned char response_frame[1100];
static size_t response_bytes, response_pos;
static unsigned char response_sequence;
static uint32_t sent_commands[8];
static unsigned command_count, data_count;

static uint64_t lume_counter_read(void)
{
    return (uint64_t)clock_ms * 1000;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

#include "../../drivers/net/cyw43455/firmware.c"

const unsigned char lume_wpa_rsn[22] = {48, 20,  1, 0, 0, 15, 172, 4,   1, 0, 0,
                                        15, 172, 4, 1, 0, 0,  15,  172, 2, 0, 0};

bool lume_wpa_set_rsn(struct lume_wpa *state, const void *data, size_t bytes)
{
    assert(state == &supplicant && bytes == sizeof(lume_wpa_rsn));
    assert(!memcmp(data, lume_wpa_rsn, bytes));
    ++rsn_inputs;
    return true;
}

int lume_wpa_input(struct lume_wpa *state, const void *data, size_t bytes,
                   struct lume_wpa_action *action)
{
    assert(state == &supplicant && bytes == 14 && ((const unsigned char *)data)[0] == 7);
    ++wpa_inputs;
    memset(action, 0, sizeof(*action));
    if (wpa_invalid)
        return -1;
    action->bytes = 14;
    action->frame[0] = 9;
    action->pairwise = action->group = action->authorize = true;
    action->gtk_id = 2;
    memset(action->tk, 0x53, sizeof(action->tk));
    memset(action->gtk, 0x71, sizeof(action->gtk));
    return 1;
}

bool lume_sdio_backplane_read(const struct lume_sdio_bus *device, uint32_t address, uint32_t *value)
{
    assert(device == &test_bus && transport_depth);
    assert(address == cores.sdio + 0x20);
    ++register_reads;
    *value = response_bytes ? 0x40 : 0;
    return !bus_fails;
}

bool lume_sdio_backplane_write(const struct lume_sdio_bus *device, uint32_t address, uint32_t value)
{
    assert(device == &test_bus && transport_depth);
    assert(address == cores.sdio + 0x20 && value == 0x40);
    return true;
}

static bool fifo_read(uint32_t argument, unsigned char *out, size_t bytes)
{
    (void)argument;
    assert(transport_depth && bytes <= sizeof(response_frame));
    ++fifo_reads;
    if (!response_bytes)
    {
        assert(bytes == 12);
        memset(out, 0, bytes);
        return true;
    }
    assert(bytes <= response_bytes - response_pos);
    memcpy(out, response_frame + response_pos, bytes);
    response_pos += bytes;
    if (response_pos == response_bytes)
        response_bytes = response_pos = 0;
    return true;
}

static bool fifo_write(uint32_t argument, const unsigned char *data, size_t bytes)
{
    (void)argument;
    assert(transport_depth && bytes >= 28 && !response_bytes);
    ++fifo_writes;
    if (write_fails)
        return false;
    if (data[5] == 2)
    {
        assert(data[18] == 9);
        ++data_count;
        return true;
    }
    assert(command_count < sizeof(sent_commands) / sizeof(sent_commands[0]));
    uint32_t command = get32(data + 12);
    sent_commands[command_count++] = command;
    if (command == 263)
    {
        assert((uint32_t)(clock_ms - data_sent_at) >= 50);
        assert(!memcmp(data + 28, "wsec_key", 9));
        unsigned index = get32(data + 37);
        assert(index == 0 || index == 2);
        assert(data[45] == (index ? 0x71 : 0x53));
    }
    return true;
}

static void unexpected_delay(unsigned microseconds)
{
    (void)microseconds;
    assert(!"The poll path must not wait for firmware");
}

static const struct lume_sdio_bus test_bus = {
    .read53 = fifo_read, .write53 = fifo_write, .delay_us = unexpected_delay};

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
    if (!stop_fails)
        hardware_active = false;
}

const struct lume_sdio_bus *lume_wifi_bus(void)
{
    return hardware_active ? &test_bus : NULL;
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
    reset_pending = stop_fails = false;
    hardware_active = true;
    authentication_reset();
    scan_id = 7;
    clock_ms = scan_started = scan_retired_at = scan_count = scan_errors = receive_count =
        receive_head = 0;
    resets = 0;
    bus = &test_bus;
    bus_fails = write_fails = wpa_invalid = frame_pending = rx_seen = false;
    register_reads = fifo_reads = fifo_writes = wpa_inputs = rsn_inputs = 0;
    response_bytes = response_pos = response_sequence = 0;
    other_interface_polls = timer_passes = command_count = data_count = 0;
    request_id = tx_sequence = flow_mask = 0;
    tx_credit = 64;
    hardware_flow = false;
    auth_head = auth_count = 0;
    memset(&supplicant, 0, sizeof(supplicant));
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

static void missing_completion_recovery(void)
{
    reset();
    struct lume_wifi_bss_info results[LUME_SCAN_MAX];
    clock_ms = 30000;
    assert(cyw_scan_result(NULL, results, LUME_SCAN_MAX) == -LUME_EIO);
    clock_ms = 89999;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN && !resets);
    clock_ms = 90000;
    stop_fails = true;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    assert(resets == 1 && hardware_active && reset_pending && scan_running && scan_retiring);
    stop_fails = false;
    assert(!cyw_scan_cancel(NULL));
    assert(resets == 2 && !hardware_active && !reset_pending && !scan_running && !scan_retiring);
    assert(!operation_depth && !transport_depth);
}

static void wrapping_retirement_deadline(void)
{
    reset();
    clock_ms = UINT32_MAX - 20000;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN);
    clock_ms += 59999;
    assert(cyw_scan_cancel(NULL) == -LUME_EAGAIN && !resets);
    ++clock_ms;
    assert(!cyw_scan_cancel(NULL) && resets == 1);
    assert(!scan_running && !scan_retiring && !operation_depth && !transport_depth);
}

static void poll_pass(void)
{
    unsigned reads = register_reads, received = fifo_reads, written = fifo_writes;
    uint32_t began = clock_ms;
    lume_wifi_poll();
    assert(clock_ms == began && !operation_depth && !transport_depth);
    assert(register_reads - reads <= 4 && fifo_reads - received <= 8 && fifo_writes - written <= 1);
    ++other_interface_polls;
    ++timer_passes;
}

static void begin_authentication(void)
{
    reset();
    scan_running = false;
    joining = authentication_ready = carrier = ssid_ok = true;
    psk_ok = false;
    join_started = clock_ms;
    auth_count = 1;
    auth_size[0] = 14;
    memset(auth_queue[0], 0, sizeof(auth_queue[0]));
    auth_queue[0][0] = 7;
    poll_pass();
    assert(pending_control.active && !pending_control.sent &&
           authentication.phase == AUTH_ASSOC_INFO);
}

static void reply(uint16_t id, int32_t status)
{
    assert(pending_control.active && pending_control.sent && !response_bytes);
    unsigned char value[22] = {0};
    size_t bytes = 0;
    if (authentication.phase == AUTH_ASSOC_INFO)
    {
        put32(value, sizeof(lume_wpa_rsn));
        bytes = 4;
    }
    else if (authentication.phase == AUTH_ASSOC_IES)
    {
        memcpy(value, lume_wpa_rsn, sizeof(value));
        bytes = sizeof(value);
    }
    response_bytes =
        lume_wifi_control_request(response_frame, sizeof(response_frame), response_sequence++, id,
                                  pending_control.command, false, value, bytes, 0);
    assert(response_bytes);
    response_frame[9] = tx_sequence + 64;
    if (status)
    {
        response_frame[20] |= 1;
        put32(response_frame + 24, (uint32_t)status);
    }
}

static void bounded_handshake(void)
{
    begin_authentication();
    for (unsigned pass = 0; pass < 80; ++pass)
    {
        if (pending_control.active && pending_control.sent && !pending_control.result)
            reply(pending_control.id, 0);
        poll_pass();
        clock_ms += 10;
        if (psk_ok && authentication.phase == AUTH_IDLE)
            break;
    }
    assert(psk_ok && !joining && operational && !resets);
    assert(wpa_inputs == 1 && rsn_inputs == 1 && data_count == 1 && command_count == 5);
    const uint32_t expected[] = {262, 262, 263, 263, 121};
    assert(!memcmp(sent_commands, expected, sizeof(expected)));
    assert(other_interface_polls == timer_passes && timer_passes > 10 && timer_passes < 80);
    assert(!pending_control.active && !authentication.action.bytes);
}

static void pending_reply_deadline(void)
{
    begin_authentication();
    poll_pass();
    assert(pending_control.sent && command_count == 1);
    uint16_t id = pending_control.id;
    assert(!lume_wifi_send(auth_queue[0], 14));
    size_t got;
    assert(control(1, false, NULL, 0, NULL, 0, &got) == -1);
    assert(pending_control.id == id && command_count == 1);
    reply(id + 1, 0);
    poll_pass();
    assert(pending_control.active && !pending_control.result && pending_control.id == id);
    for (clock_ms = 100; clock_ms < 3000; clock_ms += 100)
        poll_pass();
    assert(operational && !resets && command_count == 1);
    stop_fails = true;
    poll_pass();
    assert(!operational && reset_pending && hardware_active && resets == 1);
    assert(!pending_control.active && !authentication_ready && !authentication.action.bytes);
    stop_fails = false;
    poll_pass();
    assert(!reset_pending && !hardware_active && resets == 2);
    assert(other_interface_polls == timer_passes && timer_passes >= 30);
}

static void bounded_credit_and_errors(void)
{
    begin_authentication();
    tx_credit = 0;
    poll_pass();
    clock_ms = 1999;
    poll_pass();
    assert(operational && !fifo_writes);
    clock_ms = 2000;
    poll_pass();
    assert(!operational && resets == 1 && !fifo_writes);

    begin_authentication();
    poll_pass();
    reply(pending_control.id, -17);
    poll_pass();
    assert(!operational && resets == 1 && !pending_control.active);

    begin_authentication();
    write_fails = true;
    poll_pass();
    assert(!operational && resets == 1 && !pending_control.active);

    begin_authentication();
    poll_pass();
    bus_fails = true;
    poll_pass();
    assert(!operational && resets == 1 && !pending_control.active);
}

static void late_reply_and_explicit_stop(void)
{
    begin_authentication();
    poll_pass();
    clock_ms = 3000;
    reply(pending_control.id, 0);
    poll_pass();
    assert(!operational && resets == 1 && !pending_control.active && !wpa_inputs);

    begin_authentication();
    poll_pass();
    reply(pending_control.id, 0);
    lume_wifi_leave();
    assert(!pending_control.active && !authentication_ready && resets == 1);
    unsigned reads = register_reads;
    poll_pass();
    assert(register_reads == reads && !wpa_inputs && !operation_depth && !transport_depth);
}

int main(void)
{
    cancelled_generation();
    timeout_retains_generation();
    results_and_reset();
    missing_completion_recovery();
    wrapping_retirement_deadline();
    bounded_handshake();
    pending_reply_deadline();
    bounded_credit_and_errors();
    late_reply_and_explicit_stop();
    puts(
        "PASS bounded Wi-Fi authentication, command ownership, scan retirement and failed-stop recovery");
    return 0;
}
