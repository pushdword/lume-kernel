/* Crypto service 40 reports device capabilities separately from CPU features.
 * Only hardware operations are reported as accelerated. */
#ifndef LUME_CRYPTO_H
#define LUME_CRYPTO_H
#include <stdint.h>
#define LUME_CRYPTO_VERSION 1u
#define LUME_RANDOM_SEED_BYTES 48u
#define LUME_RANDOM_MAX 256u

enum
{
    LUME_CRYPTO_INFO = 1,
    LUME_CRYPTO_SEED,
    LUME_CRYPTO_RANDOM,
    LUME_CRYPTO_CLEAR,
    LUME_CRYPTO_AES_ENCRYPT,
    LUME_CRYPTO_AES_DECRYPT,
    LUME_CRYPTO_LIMITS,
    LUME_CRYPTO_X25519,
    LUME_CRYPTO_ED25519_BASE,
    LUME_CRYPTO_ED25519_VERIFY_POINT
};

enum
{
    LUME_CRYPTO_RANDOM_READY = 1u,
    LUME_CRYPTO_HARDWARE_ENTROPY = 2u
};

enum
{
    LUME_CRYPTO_HMAC_DRBG_SHA256 = 1u,
    LUME_CRYPTO_AES_GCM = 2u,
    LUME_CRYPTO_X25519_HW = 4u,
    LUME_CRYPTO_ED25519_HW = 8u
};

struct lume_crypto_request
{
    uint32_t version, bytes, flags, buffer, length;
};

struct lume_crypto_info
{
    uint32_t version, bytes, flags, supported, accelerated, max_random, seed_bytes, remaining,
        reseed_count;
};

/* AES-GCM: fixed96-bit nonce/full128-bit tag. A nonce must never repeat for
 * a given key. All addresses refer to the calling process. input/output may
 * coincide. Output and tag must not overlap. Failed authentication emits no bytes.
 * The optional peripheral has a4KiB packet buffer and32-byte AAD limit. */
struct lume_aead_request
{
    uint32_t version, bytes, key, key_bytes, nonce, nonce_bytes, aad, aad_bytes, input, length,
        output, tag;
};

struct lume_crypto_limits
{
    uint32_t version, bytes, max_packet, max_aad, nonce_bytes, tag_bytes, key_sizes, clock_hz;
};

/* Hardware operations require the corresponding engine. Scalar, point and output
 * buffers are32 bytes, encoded little-endian. X25519 clamps the scalar and
 * masks the input point's high bit. ED_BASE takes an already prepared scalar.
 * ED_VERIFY_POINT returns encode([scalar_b]B-[scalar_a]point); callers must
 * hash/reduce the challenge, enforce S<L and compare against R. Invalid or
 * noncanonical point encodings return EBADMSG and leave output unchanged.
 * cycles optionally points to a uint32_t engine-cycle result, or is zero.
 * Unused scalar_b/point addresses must be zero. */
struct lume_curve_request
{
    uint32_t version, bytes, scalar_a, scalar_b, point, output, cycles;
};

_Static_assert(sizeof(struct lume_curve_request) == 28, "Curve request ABI");
/* SEED requires root and exactly48 fresh unpredictable bytes from a trusted
 * entropy source (e.g. the management PC's OS CSPRNG). No entropy is credited
 * to clocks, MAC/IP addresses, CRCs or deterministic FPGA state. State is RAM
 * only: after each boot RANDOM returns EAGAIN until explicitly seeded.
 * INFO never returns key/state. Requests produce all requested bytes or return an error.
 * This profile conservatively requires reseeding after1MiB of output. */
_Static_assert(sizeof(struct lume_crypto_request) == 20, "Crypto request ABI");
_Static_assert(sizeof(struct lume_crypto_info) == 36, "Crypto info ABI");
_Static_assert(sizeof(struct lume_aead_request) == 48, "AEAD request ABI");
#endif
