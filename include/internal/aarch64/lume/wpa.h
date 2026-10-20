#ifndef LUME_WPA_H
#define LUME_WPA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_wpa
{
    unsigned char pmk[32], mac[6], peer[6], snonce[32], anonce[32], ptk[64], replay[8], gtk[16];
    unsigned gtk_id;
    bool seen, installed, gtk_valid, authorized;
    unsigned char rsn[128];
    size_t rsn_bytes;
    bool (*entropy)(void *, size_t);
};

struct lume_wpa_action
{
    unsigned char frame[512], tk[16], gtk[16], rsc[8];
    size_t bytes;
    unsigned gtk_id;
    bool pairwise, group, authorize;
};

extern const unsigned char lume_wpa_rsn[22];
void lume_secret_clear(void *, size_t);
void lume_wpa_hmac(const void *, size_t, const void *, size_t, unsigned char[20]);
bool lume_wpa_pmk(const void *, size_t, const void *, size_t, unsigned char[32]);
bool lume_wpa_unwrap(const unsigned char[16], const void *, size_t, void *, size_t *);
bool lume_wpa_begin(struct lume_wpa *, const unsigned char[6], const void *, size_t, const void *,
                    size_t, bool (*)(void *, size_t));
bool lume_wpa_set_rsn(struct lume_wpa *, const void *, size_t);
/* 0=ignore; 1=response/action; -1=invalid/authentication failed. */
int lume_wpa_input(struct lume_wpa *, const void *, size_t, struct lume_wpa_action *);
bool lume_entropy(void *, size_t);
#endif
