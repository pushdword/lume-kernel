/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM "/bin/netd"
#define BLOB "/probe.dat"
#ifndef STARTUP_BYTES
#define STARTUP_BYTES (16u * 1024u * 1024u)
#endif
#define PAGE_BYTES 4096u
#define SAMPLE_COUNT 4u

#ifdef STARTUP_INIT
#include <sys/mount.h>

int main(void)
{
    int log = open("/dev/kmsg", O_WRONLY);
    if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0)
        return 1;
    if (log > STDERR_FILENO)
        close(log);
    if (mount("proc", "/proc", "proc", 0, NULL))
        return 1;
    int setting = open("/proc/sys/kernel/printk_devkmsg", O_WRONLY);
    if (setting < 0)
        return 1;
    ssize_t written;
    do
        written = write(setting, "on\n", 3);
    while (written < 0 && errno == EINTR);
    if (close(setting) || written != 3)
        return 1;
    pid_t child = fork();
    if (!child)
    {
        char *arguments[] = {PROGRAM, NULL};
        char *environment[] = {"PATH=/bin", "LANG=C", NULL};
        execve(PROGRAM, arguments, environment);
        _exit(127);
    }
    int status = -1;
    if (child < 0 || waitpid(child, &status, 0) != child || status)
    {
        printf("STARTUP_ERROR,init,%d,%d\n", errno, status);
        fflush(stdout);
    }
    for (;;)
        pause();
}
#else
static const unsigned char image[STARTUP_BYTES]
    __attribute__((aligned(PAGE_BYTES), used)) = {[0] = 0x53, [STARTUP_BYTES - 1] = 0xa9};

struct ready
{
    uint64_t bytes, checksum;
};

static int failed(const char *operation)
{
    printf("STARTUP_ERROR,%s,%d\n", operation, errno);
    fflush(stdout);
    return 1;
}

static uint64_t nanoseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
    {
        failed("clock_gettime");
        exit(1);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int child(void)
{
    const volatile unsigned char *data = image;
    struct ready value = {sizeof(image), data[0] + data[sizeof(image) - 1]};
    return write(31, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : 1;
}

static int launch(unsigned sample)
{
    int pair[2];
    if (pipe(pair))
        return failed("pipe");
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
    {
        close(pair[0]);
        close(pair[1]);
        errno = error;
        return failed("spawn_actions_init");
    }
    if (!(error = posix_spawn_file_actions_adddup2(&actions, pair[1], 31)) &&
        !(error = posix_spawn_file_actions_addclose(&actions, pair[0])))
        error = posix_spawn_file_actions_addclose(&actions, pair[1]);
    char *arguments[] = {PROGRAM, "--child", NULL};
    char *environment[] = {"PATH=/bin", "LANG=C", NULL};
    pid_t pid = -1;
    uint64_t began = nanoseconds();
    if (!error)
        error = posix_spawn(&pid, PROGRAM, &actions, NULL, arguments, environment);
    uint64_t spawned = nanoseconds();
    posix_spawn_file_actions_destroy(&actions);
    close(pair[1]);
    struct ready value = {0};
    size_t received = 0;
    if (!error)
        while (received < sizeof(value))
        {
            ssize_t part = read(pair[0], (unsigned char *)&value + received,
                                sizeof(value) - received);
            if (part < 0 && errno == EINTR)
                continue;
            if (part <= 0)
                break;
            received += (size_t)part;
        }
    uint64_t ready_at = nanoseconds();
    close(pair[0]);
    int status = -1;
    pid_t waited = -1;
    if (!error)
        do
            waited = waitpid(pid, &status, 0);
        while (waited < 0 && errno == EINTR);
    uint64_t ended = nanoseconds();
    if (error || waited != pid || status || received != sizeof(value) ||
        value.bytes != sizeof(image) || value.checksum != 0xfc)
    {
        if (error)
            errno = error;
        return failed("launch");
    }
    printf("STARTUP,spawn,%s,%u,%llu,%llu,%llu,%llu\n", sample ? "warm" : "first", sample,
           (unsigned long long)(spawned - began), (unsigned long long)(ready_at - began),
           (unsigned long long)(ended - ready_at), (unsigned long long)value.checksum);
    fflush(stdout);
    return 0;
}

static unsigned byte_value(uint64_t offset)
{
    return (unsigned)((offset * 17 + offset / PAGE_BYTES * 3 + 11) & 255);
}

static int mappings(void)
{
    int fd = open(BLOB, O_RDONLY);
    struct stat metadata;
    if (fd < 0 || fstat(fd, &metadata) || metadata.st_size != STARTUP_BYTES)
    {
        if (fd >= 0)
            close(fd);
        return failed("blob");
    }
    uint64_t expected = byte_value(STARTUP_BYTES - 1);
    for (uint64_t offset = 0; offset < STARTUP_BYTES; offset += PAGE_BYTES)
        expected += byte_value(offset);
    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        uint64_t began = nanoseconds();
        void *mapping = mmap(NULL, STARTUP_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
        uint64_t mapped = nanoseconds();
        if (mapping == MAP_FAILED)
        {
            close(fd);
            return failed("mmap");
        }
        const volatile unsigned char *data = mapping;
        uint64_t checksum = 0;
        for (uint64_t offset = 0; offset < STARTUP_BYTES; offset += PAGE_BYTES)
            checksum += data[offset];
        checksum += data[STARTUP_BYTES - 1];
        uint64_t touched = nanoseconds();
        int error = munmap(mapping, STARTUP_BYTES);
        uint64_t unmapped = nanoseconds();
        if (error || checksum != expected)
        {
            close(fd);
            return failed("munmap_or_checksum");
        }
        printf("STARTUP,mmap,%s,%u,%llu,%llu,%llu,%llu\n", sample ? "warm" : "first", sample,
               (unsigned long long)(mapped - began), (unsigned long long)(touched - mapped),
               (unsigned long long)(unmapped - touched), (unsigned long long)checksum);
        fflush(stdout);
    }
    close(fd);
    return 0;
}

#ifdef LUME_STARTUP_APP
int app_main(int argc, char **argv, char **environment)
#else
int main(int argc, char **argv)
#endif
{
#ifdef LUME_STARTUP_APP
    (void)environment;
#endif
    if (argc == 2 && !strcmp(argv[1], "--child"))
        return child();
    if (sizeof(image) % PAGE_BYTES)
        return failed("fixture_alignment");
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution))
        return failed("clock_getres");
    uint64_t clock_resolution = (uint64_t)resolution.tv_sec * UINT64_C(1000000000) +
                                (uint64_t)resolution.tv_nsec;
    printf("STARTUP_ENV,bytes=%u,samples=%u,page_bytes=%u,storage=ram,clock_resolution_ns=%llu\n",
           STARTUP_BYTES, SAMPLE_COUNT, PAGE_BYTES, (unsigned long long)clock_resolution);
    fflush(stdout);
    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample)
        if (launch(sample))
            return 1;
    if (mappings())
        return 1;
    puts("PASS_STARTUP_MMAP_DONE");
    fflush(stdout);
    return 0;
}
#endif
