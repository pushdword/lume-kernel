#include "lume/uart_input.h"
#include <assert.h>
#include <stdio.h>
static unsigned checks;
#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)

static void activate(struct lume_uart_input *s)
{
    const char *p = LUME_UART_ENABLE_TOKEN;
    while (*p)
        T(lume_uart_input_feed(s, (unsigned char)*p++) == -LUME_EAGAIN);
}

int main(void)
{
    struct lume_uart_input s = {0};
    for (unsigned i = 0; i < 4096; ++i)
    {
        T(lume_uart_input_feed(&s, i & 255) == -LUME_EAGAIN);
        T(!s.enabled && !s.bytes);
    }
    const char *p = LUME_UART_ENABLE_TOKEN;
    for (unsigned n = 0; p[n + 1]; ++n)
        T(lume_uart_input_feed(&s, p[n]) == -LUME_EAGAIN && !s.enabled);
    T(lume_uart_input_feed(&s, 'X') == -LUME_EAGAIN && !s.enabled);
    T(lume_uart_input_feed(&s, 'L') == -LUME_EAGAIN);
    T(lume_uart_input_feed(&s, 0x100 | 'U') == -LUME_EAGAIN && s.matched == 0 && s.errors == 0);
    activate(&s);
    T(s.enabled && !s.bytes && !s.errors);
    for (unsigned c = 0; c < 256; ++c)
        T(lume_uart_input_feed(&s, c) == (int)c);
    T(s.bytes == 256);
    for (unsigned c = 0x100; c <= 0x800; c <<= 1)
        T(lume_uart_input_feed(&s, c | 'x') == -LUME_EAGAIN);
    T(s.errors == 4 && s.bytes == 256 && s.enabled);
    T(lume_uart_input_feed(&s, 'a') == 'a' && s.bytes == 257);
    struct lume_uart_input overlap = {0};
    T(lume_uart_input_feed(&overlap, 'L') == -LUME_EAGAIN);
    activate(&overlap);
    T(overlap.enabled);
    printf("PASS_UART_INPUT_OPT_IN checks=%u\n", checks);
}
