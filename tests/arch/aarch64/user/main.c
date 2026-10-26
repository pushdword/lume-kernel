#include <stdint.h>
#include <stddef.h>
static volatile uint64_t bss[32];
static volatile uint64_t initialized = UINT64_C(0x454c463641413634);

static int64_t call(uint64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0") = a;
    register uint64_t x1 __asm__("x1") = b;
    register uint64_t x2 __asm__("x2") = c;
    register uint64_t x8 __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

int user_main(uint64_t pid, volatile uint64_t *private, uint64_t deadline)
{
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if ((sp & 15) || initialized != UINT64_C(0x454c463641413634))
        return 10;
    for (unsigned i = 0; i < 32; ++i)
        if (bss[i])
            return 11;
    if (call(99, 0, 0, 0) != -38 || call(3, 1, 0x200000, 16) != -14)
        return 12;
    const char *message = pid == 1 ? "LUME ELF64 PID1\n" : "LUME ELF64 PID2\n";
    if (call(3, 1, (uintptr_t)message, 16) != 16)
        return 13;
    uint64_t iterations = 0, canary = private[1];
    do
    {
        bss[pid] = ++iterations;
        private[0] = iterations;
        if (call(1, 0, 0, 0) != (int64_t)pid || private[1] != canary || bss[pid] != iterations)
            return 14;
    } while (call(2, 0, 0, 0) < (int64_t)deadline);
    return 0;
}
