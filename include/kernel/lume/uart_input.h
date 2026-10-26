#ifndef LUME_UART_INPUT_H
#define LUME_UART_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#include "lume/abi.h"

/* Explicit opt-in on the physical RX wire. Until this complete sequence is
 * received, unconnected GPIO noise must never enter the shell. */
#define LUME_UART_ENABLE_TOKEN "LUME-ENABLE-UART\n"

struct lume_uart_input
{
    unsigned matched;
    bool enabled;
    uint64_t bytes, errors;
};

static inline int lume_uart_input_feed(struct lume_uart_input *s, uint32_t word)
{
    if (word & 0xf00)
    {
        s->matched = 0;
        if (s->enabled)
            ++s->errors;
        return -LUME_EAGAIN;
    }
    unsigned char c = (unsigned char)word;
    if (!s->enabled)
    {
        static const char token[] = LUME_UART_ENABLE_TOKEN;
        s->matched = c == (unsigned char)token[s->matched] ? s->matched + 1
                     : c == (unsigned char)token[0]        ? 1
                                                           : 0;
        if (s->matched == sizeof(token) - 1)
        {
            s->enabled = true;
            s->matched = 0;
        }
        return -LUME_EAGAIN;
    }
    ++s->bytes;
    return c;
}
#endif
