#include <stdint.h>
#include <stdbool.h>

uint64_t __udivmoddi4(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    if (!divisor)
        __builtin_trap();
    uint64_t quotient = 0, rest = 0;
    for (unsigned i = 64; i; --i)
    {
        bool carry = (rest >> 63) != 0;
        rest = (rest << 1) | ((dividend >> (i - 1)) & 1);
        if (carry || rest >= divisor)
        {
            rest -= divisor;
            quotient |= UINT64_C(1) << (i - 1);
        }
    }
    if (remainder)
        *remainder = rest;
    return quotient;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return __udivmoddi4(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t remainder;
    __udivmoddi4(a, b, &remainder);
    return remainder;
}

int64_t __divdi3(int64_t a, int64_t b)
{
    uint64_t dividend = a < 0 ? -(uint64_t)a : (uint64_t)a;
    uint64_t divisor = b < 0 ? -(uint64_t)b : (uint64_t)b;
    uint64_t result = __udivmoddi4(dividend, divisor, 0);
    return (int64_t)((a < 0) != (b < 0) ? -result : result);
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t dividend = a < 0 ? -(uint64_t)a : (uint64_t)a;
    uint64_t divisor = b < 0 ? -(uint64_t)b : (uint64_t)b, remainder;
    __udivmoddi4(dividend, divisor, &remainder);
    return (int64_t)(a < 0 ? -remainder : remainder);
}
