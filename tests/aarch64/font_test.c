#include "font5x7.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    for (unsigned c = 33; c < 127; ++c)
    {
        unsigned ink = 0;
        for (unsigned x = 0; x < 5; ++x)
        {
            unsigned column = lume_font_column(c, x);
            assert(!(column & 128));
            ink |= column;
        }
        assert(ink && "Printable ASCII glyph is empty");
        assert(!lume_font_column(c, 5));
    }
    for (unsigned x = 0; x < 5; ++x)
    {
        assert(!lume_font_column(' ', x));
        assert(lume_font_column('{', x) == lume_font_column('}', 4 - x));
    }
    puts("PASS_FONT printable ASCII coverage/braces/column bounds");
}
