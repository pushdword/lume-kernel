/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/platform_driver.h"
#include "lume/vfs.h"
#include "lume/file_mode.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform_util.h"
#include <limits.h>
#include <string.h>

#define SEED_BYTES 48u
static mbedtls_ctr_drbg_context generator;
static unsigned char seed[SEED_BYTES];
static bool seeded, seed_available;

static int seed_entropy(void *context, unsigned char *output, size_t bytes)
{
    (void)context;
    if (!seed_available || bytes != sizeof(seed))
        return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    memcpy(output, seed, bytes);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    seed_available = false;
    return 0;
}

static bool entropy(void *output, size_t bytes)
{
    if (!seeded || !output || !bytes || bytes > 256)
        return false;
    if (!mbedtls_ctr_drbg_random(&generator, output, bytes))
        return true;
    mbedtls_platform_zeroize(output, bytes);
    return false;
}

static const struct lume_platform_entropy_ops operations = {entropy};

int lume_pc_entropy_start(void)
{
    if (seeded)
        return 0;
    struct lume_files files;
    lume_files_init(&files);
    int result = -LUME_EIO;
    int64_t fd = lume_vfs_open(&files, "/data/system/rngseed.bin", LUME_O_RDWR);
    struct lume_stat metadata;
    if (fd < 0)
    {
        result = (int)fd;
        goto done;
    }
    struct lume_file_mode_request permissions = {.version = LUME_FILE_MODE_VERSION,
                                                 .bytes = sizeof(permissions),
                                                 .fd = (int)fd,
                                                 .mode = 0600};
    if (lume_vfs_stat(&files, fd, &metadata) || metadata.kind != LUME_NODE_FILE ||
        metadata.size != sizeof(seed) ||
        lume_vfs_file_mode(&files, LUME_FILE_MODE_FD, &permissions, NULL) ||
        lume_vfs_read(&files, fd, seed, sizeof(seed)) != sizeof(seed))
        goto done;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < sizeof(seed); ++i)
        nonzero |= seed[i];
    if (!nonzero)
        goto done;
    mbedtls_ctr_drbg_init(&generator);
    mbedtls_ctr_drbg_set_entropy_len(&generator, sizeof(seed));
    mbedtls_ctr_drbg_set_reseed_interval(&generator, INT_MAX - 1);
    seed_available = true;
    if (mbedtls_ctr_drbg_seed(&generator, seed_entropy, NULL, (const unsigned char *)"Lume PC", 7))
        goto failed;
    /* Advance and persist the next boot's seed before exposing any random output. */
    if (mbedtls_ctr_drbg_random(&generator, seed, sizeof(seed)) ||
        lume_vfs_seek(&files, fd, 0, LUME_SEEK_SET) ||
        lume_vfs_write(&files, fd, seed, sizeof(seed)) != sizeof(seed) || lume_vfs_sync(&files, fd))
        goto failed;
    result = lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations));
    if (result)
        goto failed;
    seeded = true;
    goto done;
failed:
    mbedtls_ctr_drbg_free(&generator);
done:
    seed_available = false;
    mbedtls_platform_zeroize(seed, sizeof(seed));
    lume_files_release(&files);
    return result;
}
