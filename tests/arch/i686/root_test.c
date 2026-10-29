#include "lume/vfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && !fseek(input, 0, SEEK_SET));
    unsigned char *image = malloc((size_t)size);
    assert(image && fread(image, 1, (size_t)size, input) == (size_t)size);
    assert(!fclose(input));
    static const char shell[] = "embedded shell";
    lume_vfs_init(shell, sizeof(shell));
    assert(lume_vfs_mount_fat(image, (size_t)size));
    struct lume_files files;
    lume_files_init(&files);
    char data[64];
    int fd = (int)lume_vfs_open(&files, "/bin/sh", LUME_O_RDONLY);
    assert(fd >= 0 && lume_vfs_read(&files, fd, data, sizeof(data)) == sizeof(shell));
    assert(!memcmp(data, shell, sizeof(shell)) && !lume_vfs_close(&files, fd));
    fd = (int)lume_vfs_open(&files, "/bin/ls", LUME_O_RDONLY);
    assert(fd >= 0 && lume_vfs_read(&files, fd, data, 20) == 20);
    assert(!memcmp(data, "\177ELF\1", 5) && (unsigned char)data[18] == 3);
    assert(!lume_vfs_close(&files, fd));
    fd = (int)lume_vfs_open(&files, "/usr/libexec/vidtest", LUME_O_RDONLY);
    assert(fd >= 0 && lume_vfs_read(&files, fd, data, 20) == 20);
    assert(!memcmp(data, "\177ELF\1", 5) && (unsigned char)data[18] == 3);
    assert(!lume_vfs_close(&files, fd));
    puts("PASS i686 application root import, embedded shell preservation and ELF32 utility");
    free(image);
    return 0;
}
