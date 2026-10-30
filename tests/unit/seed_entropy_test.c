/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/random/seed_file/entropy.c"

static unsigned char stored[SEED_BYTES];
static unsigned released, writes, synchronizations;
static int failure;
static const struct lume_platform_entropy_ops *registered;

void mbedtls_platform_zeroize(void *data, size_t bytes)
{
    volatile unsigned char *p = data;
    while (bytes--)
        *p++ = 0;
}

void lume_files_init(struct lume_files *files)
{
    memset(files, 0, sizeof(*files));
}

void lume_files_release(struct lume_files *files)
{
    (void)files;
    ++released;
}

int64_t lume_vfs_open(struct lume_files *files, const char *path, uint64_t flags)
{
    (void)files;
    assert(!strcmp(path, "/data/system/rngseed.bin") && flags == LUME_O_RDWR);
    return failure == 1 ? -LUME_ENOENT : 3;
}

int64_t lume_vfs_stat(struct lume_files *files, uint64_t fd, struct lume_stat *metadata)
{
    (void)files;
    assert(fd == 3);
    *metadata =
        (struct lume_stat){.kind = LUME_NODE_FILE, .size = failure == 2 ? 47 : sizeof(stored)};
    return 0;
}

int64_t lume_vfs_file_mode(struct lume_files *files, unsigned operation,
                           struct lume_file_mode_request *request, const char *path)
{
    (void)files;
    assert(operation == LUME_FILE_MODE_FD && request->fd == 3 && request->mode == 0600 && !path);
    return failure == 3 ? -LUME_EIO : 0;
}

int64_t lume_vfs_read(struct lume_files *files, uint64_t fd, void *data, uint64_t bytes)
{
    (void)files;
    assert(fd == 3 && bytes == sizeof(stored));
    memcpy(data, stored, sizeof(stored));
    return failure == 4 ? 47 : sizeof(stored);
}

int64_t lume_vfs_seek(struct lume_files *files, uint64_t fd, int64_t offset, uint64_t whence)
{
    (void)files;
    assert(fd == 3 && !offset && whence == LUME_SEEK_SET);
    return 0;
}

int64_t lume_vfs_write(struct lume_files *files, uint64_t fd, const void *data, uint64_t bytes)
{
    (void)files;
    assert(fd == 3 && bytes == sizeof(stored));
    ++writes;
    memcpy(stored, data, sizeof(stored));
    return failure == 5 ? 47 : sizeof(stored);
}

int64_t lume_vfs_sync(struct lume_files *files, uint64_t fd)
{
    (void)files;
    assert(fd == 3);
    ++synchronizations;
    return failure == 6 ? -LUME_EIO : 0;
}

int lume_platform_driver_register(unsigned kind, const void *operations, size_t bytes)
{
    assert(kind == LUME_PLATFORM_ENTROPY && bytes == sizeof(*registered));
    assert(writes && synchronizations);
    if (failure == 7)
        return -LUME_EBUSY;
    registered = operations;
    return 0;
}

static void reset(int error)
{
    mbedtls_ctr_drbg_free(&generator);
    seeded = seed_available = false;
    registered = NULL;
    failure = error;
    writes = synchronizations = released = 0;
    for (unsigned i = 0; i < sizeof(stored); ++i)
        stored[i] = (unsigned char)(i + 1);
}

int main(void)
{
    assert(!mbedtls_ctr_drbg_self_test(0));
    unsigned char first[32], second[32], boot_seed[SEED_BYTES];
    for (int error = 1; error <= 7; ++error)
    {
        reset(error);
        assert(lume_pc_entropy_start() < 0 && !seeded && !registered && released == 1);
        assert(!entropy(first, sizeof(first)));
        for (unsigned i = 0; i < sizeof(seed); ++i)
            assert(!seed[i]);
    }
    reset(0);
    memset(stored, 0, sizeof(stored));
    assert(lume_pc_entropy_start() < 0 && !seeded);
    reset(0);
    memcpy(boot_seed, stored, sizeof(stored));
    assert(!lume_pc_entropy_start() && seeded && registered && released == 1);
    assert(memcmp(stored, boot_seed, sizeof(stored)));
    assert(registered->lume_entropy(first, sizeof(first)));
    assert(registered->lume_entropy(second, sizeof(second)));
    assert(memcmp(first, second, sizeof(first)));
    assert(!lume_pc_entropy_start() && writes == 1 && released == 1);
    assert(!entropy(first, 0) && !entropy(first, 257));
    memcpy(boot_seed, stored, sizeof(stored));
    seeded = false;
    mbedtls_ctr_drbg_free(&generator);
    assert(!lume_pc_entropy_start() && memcmp(stored, boot_seed, sizeof(stored)));
    assert(entropy(second, sizeof(second)) && memcmp(first, second, sizeof(first)));
    puts("PASS seeded CTR_DRBG vectors, seed rotation, persistence failures and resource release");
}
