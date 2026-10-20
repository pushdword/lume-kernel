#ifndef LUME_CRYPTO_DEVICE_H
#define LUME_CRYPTO_DEVICE_H
#include <stdint.h>
#define LUME_AES_PACKET_MAX 4096u
#define LUME_AES_AAD_MAX 32u
struct lume_crypto_device {uint32_t base,clock_hz,available;};
/* Private kernel pointers only. Driver overwrites data only after verifying
 * auth_ok. On encryption tag is output; on decryption it is expected input. */
struct lume_aes_packet {unsigned key_bytes,aad_bytes,bytes,decrypt;
    unsigned char key[32],nonce[12],aad[32],tag[16];unsigned char *data;};
int lume_crypto_device_discover(struct lume_crypto_device *,uint32_t);
int lume_crypto_device_packet(struct lume_crypto_device *,struct lume_aes_packet *);
int lume_crypto_device_selftest(struct lume_crypto_device *);
struct lume_curve_packet {unsigned operation;unsigned char scalar_a[32],scalar_b[32],point[32],output[32];uint32_t cycles;};
int lume_curve_device_discover(struct lume_crypto_device *,uint32_t);
int lume_curve_device_packet(struct lume_crypto_device *,struct lume_curve_packet *);
int lume_curve_device_selftest(struct lume_crypto_device *);
uint32_t lume_crypto_now(void);
void lume_crypto_checkpoint(void);
#endif
