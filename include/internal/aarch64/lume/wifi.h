#ifndef LUME_WIFI_H
#define LUME_WIFI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_wifi_cores
{
    uint32_t common, arm, arm_wrap, d11_wrap, sdio, ram_bytes;
    unsigned arm_revision, sdio_revision;
};

bool lume_wifi_erom(const unsigned char *, size_t, struct lume_wifi_cores *);
size_t lume_wifi_nvram(const unsigned char *, size_t, unsigned char *, size_t);
uint32_t lume_wifi_crc32(uint32_t, const void *, size_t);
size_t lume_wifi_control_request(unsigned char *, size_t, unsigned char, uint16_t, uint32_t, bool,
                                 const void *, size_t, size_t);

struct lume_wifi_frame
{
    unsigned char sequence, channel, credit, flow;
    size_t length, offset;
};

bool lume_wifi_frame_header(const unsigned char *, size_t, struct lume_wifi_frame *);
bool lume_wifi_tx_allowed(unsigned char, unsigned char, unsigned char, bool);
/* 1=matching response; 0=other packet/id; -1=malformed; -2=firmware error. */
int lume_wifi_control_reply(const unsigned char *, size_t, uint16_t, uint32_t, void *, size_t,
                            size_t *, int32_t *);
bool lume_wifi_firmware(void);
/* Pi backend entry points. Common code uses netif.h/wireless.h instead. */
struct lume_access_point;
bool lume_pi5_wifi_register(const unsigned char[6]);
bool lume_wifi_prepare(const unsigned char[6]);
void lume_wifi_poll(void);
int lume_wifi_receive(void *, size_t);
bool lume_wifi_send(const void *, size_t);
bool lume_wifi_link(void);
int lume_wifi_scan(struct lume_access_point *, size_t);
bool lume_wifi_join(const unsigned char *, size_t, const unsigned char *, size_t);
void lume_wifi_leave(void);
#endif
