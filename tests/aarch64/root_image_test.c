#include "lume/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FAIL root image line %u: %s\n", __LINE__, #condition);                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

void lume_console_putc(char c)
{
    (void)c;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && strcmp(argv[2], "--reject") == 0));
    FILE *file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length >= 512 && length <= 64 * 1024 * 1024);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *image = malloc((size_t)length);
    CHECK(image != NULL && fread(image, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    static const unsigned char shell[] = "embedded-shell";
    unsigned char *staging = malloc(1024 * 1024);
    CHECK(staging != NULL);
    lume_vfs_program_buffer(staging, 1024 * 1024);
    lume_vfs_init(shell, sizeof(shell));
    bool mounted = lume_vfs_mount_fat(image, (size_t)length);
    CHECK(mounted == (argc == 2));
    struct lume_files files;
    lume_files_init(&files);
    const void *program;
    size_t bytes;
    CHECK(lume_vfs_program(&files, "/bin/sh", &program, &bytes) == 0);
    CHECK(program == shell && bytes == sizeof(shell));
    if (argc == 3)
        CHECK(lume_vfs_program(&files, "/bin/busybox", &program, &bytes) == -LUME_ENOENT);
    else
    {
        static const char *const names[] = {
            "busybox", "net",  "cat", "echo",  "false", "true", "pwd", "printenv", "basename",
            "dirname", "head", "wc",  "mkdir", "rmdir", "ls",   "cp",  "touch",    "update"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        {
            char path[64];
            CHECK(snprintf(path, sizeof(path), "/bin/%s", names[i]) > 0);
            CHECK(lume_vfs_program(&files, path, &program, &bytes) == 0 && bytes >= 64);
            const unsigned char *p = program;
            CHECK(memcmp(p,
                         "\x7f"
                         "ELF",
                         4) == 0 &&
                  p[4] == 2 && p[5] == 1 && p[18] == 183 && p[19] == 0);
        }
        CHECK(lume_vfs_program(&files, "/usr/bin/htop", &program, &bytes) == 0 && bytes > 64);
        CHECK(lume_vfs_program(&files, "/usr/bin/vi", &program, &bytes) == 0 && bytes > 64);
        CHECK(lume_vfs_program(&files, "/usr/libexec/libctest", &program, &bytes) == 0 &&
              bytes > 64);
        int64_t fd = lume_vfs_open(&files, "/lib/firmware/wifi.bin", LUME_O_RDONLY);
        CHECK(fd >= 3);
        struct lume_stat stat;
        CHECK(lume_vfs_stat(&files, (uint64_t)fd, &stat) == 0 && stat.size > 0);
        CHECK(lume_vfs_close(&files, (uint64_t)fd) == 0);
        fd = lume_vfs_open(&files, "/etc/bootmode", LUME_O_RDONLY);
        if (fd >= 0)
        {
            char mode[12] = {0};
            CHECK(lume_vfs_read(&files, (uint64_t)fd, mode, 11) == 11 &&
                  strcmp(mode, "standalone\n") == 0);
            CHECK(lume_vfs_close(&files, (uint64_t)fd) == 0);
        }
    }
    lume_files_release(&files);
    free(staging);
    free(image);
    printf("PASS_ROOT_IMAGE_VFS checks=%u mounted=%u\n", checks, (unsigned)mounted);
    return 0;
}
