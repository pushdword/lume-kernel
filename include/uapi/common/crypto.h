/* System call 40 exposes cryptographic operations. Device discovery reports hardware
 * capabilities separately from CPU instruction features. Software results are not reported as
 * hardware acceleration. */
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

/* AES-GCM uses a 96-bit nonce and a 128-bit tag. A key must never reuse a nonce. All addresses
 * belong to the calling process. Input and output buffers may coincide, but the output and tag
 * buffers must not overlap. Authentication failure produces no output bytes. The optional
 * peripheral supports packets up to 4 KiB and additional authenticated data up to 32 bytes. */
struct lume_aead_request
{
    uint32_t version, bytes, key, key_bytes, nonce, nonce_bytes, aad, aad_bytes, input, length,
        output, tag;
};

struct lume_crypto_limits
{
    uint32_t version, bytes, max_packet, max_aad, nonce_bytes, tag_bytes, key_sizes, clock_hz;
};

/* The hardware performs each complete operation without a software fallback. Scalar, point and
 * output buffers contain 32 bytes in little-endian order. X25519 clamps the scalar and masks
 * the high bit of the input point. ED_BASE takes a prepared scalar. ED_VERIFY_POINT returns
 * encode([scalar_b]B-[scalar_a]point). The caller must hash and reduce the challenge, enforce S
 * < L and compare the result with R. Invalid or noncanonical point encodings return EBADMSG
 * without changing the output. A nonzero cycles field points to a uint32_t result containing
 * engine cycles. Unused scalar_b and point addresses must be zero. */
struct lume_curve_request
{
    uint32_t version, bytes, scalar_a, scalar_b, point, output, cycles;
};

_Static_assert(sizeof(struct lume_curve_request) == 28, "Curve request ABI");
/* SEED requires root and exactly 48 fresh, unpredictable bytes from a trusted entropy source
 * such as an operating-system CSPRNG. Clocks, MAC addresses, IP addresses, CRCs and
 * deterministic FPGA state do not supply entropy. Generator state exists only in RAM. After
 * boot, RANDOM returns EAGAIN until SEED initializes that state. INFO does not expose keys or
 * generator state. Random requests have a fixed size limit and return the complete output or an
 * error. The generator requires reseeding after 1 MiB of output. */
_Static_assert(sizeof(struct lume_crypto_request) == 20, "Crypto request ABI");
_Static_assert(sizeof(struct lume_crypto_info) == 36, "Crypto info ABI");
_Static_assert(sizeof(struct lume_aead_request) == 48, "AEAD request ABI");
#endif
