#include <assert.h>
#include <stdint.h>
#include <stdio.h>
uint64_t __udivmoddi4(uint64_t, uint64_t, uint64_t *);
int64_t __divdi3(int64_t, int64_t);
int64_t __moddi3(int64_t, int64_t);

static uint64_t state = UINT64_C(0xcafebabe12345678);

static uint64_t random_word(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

int main(void)
{
    const uint64_t corners[] = {
        0, 1, 2, 3, UINT32_MAX, UINT64_C(1) << 32, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX};
    for (unsigned i = 0; i < 100000; ++i)
    {
        uint64_t a = i < 81 ? corners[i / 9] : random_word();
        uint64_t b = i < 81 ? corners[i % 9] : random_word();
        if (!b)
            b = 1;
        uint64_t remainder;
        assert(__udivmoddi4(a, b, &remainder) == a / b);
        assert(remainder == a % b);
        int64_t sa = (int64_t)a, sb = (int64_t)b;
        if (sa == INT64_MIN && sb == -1)
            continue;
        assert(__divdi3(sa, sb) == sa / sb);
        assert(__moddi3(sa, sb) == sa % sb);
    }
    puts("PASS 100000 signed and unsigned 64-bit division cases");
    return 0;
}
