/* Small bounded VT100-style renderer for the existing fixed TXT1 controller.
 * It never changes scan timing, font ROM, or the bytes mirrored to UART. */
#include "terminal.h"

static unsigned color(struct terminal *t)
{
    unsigned f = t->foreground, b = t->background;
    if (t->inverse)
    {
        unsigned n = f;
        f = b;
        b = n;
    }
    return (b << 12) | (f << 8);
}

unsigned terminal_cell(const struct terminal *t, unsigned at)
{
    if (t->watch_active && !t->watch_touched && at == t->watch_at)
        return t->watch_cell;
    return at >= t->erase_first && at < t->erase_end ? t->erase_blank
                                                     : terminal_mmio_read(t->cells + at);
}

static void touch(struct terminal *t, unsigned first, unsigned end)
{
    if (t->watch_active && t->watch_at >= first && t->watch_at < end)
        t->watch_touched = 1;
}

static void blank_range(struct terminal *t, unsigned first, unsigned end, unsigned blank)
{
    touch(t, first, end);
    volatile uint32_t *p = t->cells + first;
    unsigned n = end - first;
    while (n >= 4)
    {
        terminal_mmio_write(p, blank);
        terminal_mmio_write(p + 1, blank);
        terminal_mmio_write(p + 2, blank);
        terminal_mmio_write(p + 3, blank);
        p += 4;
        n -= 4;
    }
    while (n--)
        terminal_mmio_write(p++, blank);
}

static void flush_erase(struct terminal *t)
{
    blank_range(t, t->erase_first, t->erase_end, t->erase_blank);
    t->erase_first = t->erase_end = 0;
}

void terminal_begin(struct terminal *t)
{
    t->batch = 1;
    t->watch_touched = 0;
}

void terminal_end(struct terminal *t)
{
    flush_erase(t);
    t->batch = 0;
}

static void cover_erase(struct terminal *t, unsigned at, unsigned count)
{
    unsigned first = t->erase_first, end = t->erase_end;
    if (at < end && at + count > first)
    {
        if (at > first)
            blank_range(t, first, at, t->erase_blank);
        t->erase_first = at + count < end ? at + count : end;
    }
}

static void cell_store(struct terminal *t, unsigned at, unsigned value)
{
    cover_erase(t, at, 1);
    touch(t, at, at + 1);
    terminal_mmio_write(t->cells + at, value);
}

static void erase(struct terminal *t, unsigned first, unsigned end)
{
    unsigned blank = t->attribute | ' ';
    if (!t->batch)
    {
        blank_range(t, first, end, blank);
        return;
    }
    if (first == end)
        return;
    if (t->erase_first != t->erase_end)
    {
        if (blank == t->erase_blank && first <= t->erase_end && end >= t->erase_first)
        {
            if (first < t->erase_first)
                t->erase_first = first;
            if (end > t->erase_end)
                t->erase_end = end;
            return;
        }
        flush_erase(t);
    }
    t->erase_first = first;
    t->erase_end = end;
    t->erase_blank = blank;
}

void terminal_start(struct terminal *t, volatile uint32_t *cells, unsigned columns, unsigned rows)
{
    *t = (struct terminal){0};
    t->cells = cells;
    t->foreground = 15;
    t->attribute = 0x0f00;
    t->columns = columns;
    t->rows = rows;
    erase(t, 0, rows * columns);
}

static void down(struct terminal *t)
{
    unsigned rows = t->rows, columns = t->columns;
    if (++t->row < rows)
        return;
    t->row = rows - 1;
    unsigned words = (rows - 1) * columns;
    /* Never copy the cursor overlay into the next row during a scroll. */
    if (t->watch_active && !t->watch_touched)
    {
        terminal_mmio_write(t->cells + t->watch_at, t->watch_cell);
        t->watch_touched = 1;
    }
    flush_erase(t);
    volatile uint32_t *to = t->cells, *from = to + columns;
    /* TXT1 has one DWORD address per cell. String MMIO operations are forbidden. */
    unsigned left = words;
    while (left >= 4)
    {
        terminal_mmio_write(to, terminal_mmio_read(from));
        terminal_mmio_write(to + 1, terminal_mmio_read(from + 1));
        terminal_mmio_write(to + 2, terminal_mmio_read(from + 2));
        terminal_mmio_write(to + 3, terminal_mmio_read(from + 3));
        to += 4;
        from += 4;
        left -= 4;
    }
    while (left--)
        terminal_mmio_write(to++, terminal_mmio_read(from++));
    erase(t, words, rows * columns);
}

static unsigned parameter(struct terminal *t, unsigned i, unsigned fallback)
{
    return i <= t->count && t->parameters[i] ? t->parameters[i] : fallback;
}

static unsigned limit(unsigned n, unsigned max)
{
    return n < max ? n : max - 1;
}

static void command(struct terminal *t, unsigned c)
{
    unsigned n = parameter(t, 0, 1), at = t->row * t->columns + t->column;
    if (t->private_mode)
    {
        if (c == 'h' || c == 'l')
            for (unsigned i = 0; i <= t->count; i++)
                if (t->parameters[i] == 25)
                    t->cursor_hidden = c == 'l';
        return; /* Other private modes remain consumed without debris. */
    }
    switch (c)
    {
    case 'H':
    case 'f':
        t->row = limit(parameter(t, 0, 1) - 1, t->rows);
        t->column = limit(parameter(t, 1, 1) - 1, t->columns);
        break;
    case 'A':
        t->row = n > t->row ? 0 : t->row - n;
        break;
    case 'B':
        t->row = limit(t->row + n, t->rows);
        break;
    case 'C':
        t->column = limit(t->column + n, t->columns);
        break;
    case 'D':
        t->column = n > t->column ? 0 : t->column - n;
        break;
    case 'G':
        t->column = limit(n - 1, t->columns);
        break;
    case 'd':
        t->row = limit(n - 1, t->rows);
        break;
    case 'J':
        if (t->parameters[0] == 0)
            erase(t, at, t->rows * t->columns);
        else if (t->parameters[0] == 1)
            erase(t, 0, at + 1);
        else if (t->parameters[0] == 2)
            erase(t, 0, t->rows * t->columns);
        break;
    case 'K':
        if (t->parameters[0] == 0)
            erase(t, at, (t->row + 1) * t->columns);
        else if (t->parameters[0] == 1)
            erase(t, t->row * t->columns, at + 1);
        else if (t->parameters[0] == 2)
            erase(t, t->row * t->columns, (t->row + 1) * t->columns);
        break;
    case 's':
        t->saved_row = t->row;
        t->saved_column = t->column;
        break;
    case 'u':
        t->row = t->saved_row;
        t->column = t->saved_column;
        break;
    case 'm':
        for (unsigned i = 0; i <= t->count; i++)
        {
            unsigned a = t->parameters[i]; /* ANSI RGB order -> TXT1 BGR bits. */
            unsigned k = a % 10, kb = ((k & 1) << 2) | (k & 2) | ((k & 4) >> 2);
            if (a == 0)
            {
                t->foreground = 15;
                t->background = 0;
                t->inverse = 0;
            }
            else if (a == 1)
                t->foreground |= 8;
            else if (a == 22)
                t->foreground &= 7;
            else if (a == 7)
                t->inverse = 1;
            else if (a == 27)
                t->inverse = 0;
            else if (a == 39)
                t->foreground = 15;
            else if (a == 49)
                t->background = 0;
            else if (a >= 30 && a <= 37)
                t->foreground = kb;
            else if (a >= 40 && a <= 47)
                t->background = kb;
            else if (a >= 90 && a <= 97)
                t->foreground = kb | 8;
            else if (a >= 100 && a <= 107)
                t->background = kb | 8;
        }
        t->attribute = color(t);
        break;
    default:
        break;
    }
    t->wrap = 0;
}

void terminal_byte(struct terminal *t, unsigned c)
{
    if (t->utf8_left)
    {
        if ((c & 0xc0) == 0x80)
        {
            t->utf8 = (t->utf8 << 6) | (c & 63);
            if (--t->utf8_left)
                return;
            c = t->utf8 == 0x20ac ? 0x80 : t->utf8 >= t->utf8_min && t->utf8 <= 255 ? t->utf8 : '?';
        }
        else
        {
            t->utf8_left = 0;
            terminal_byte(t, '?');
            terminal_byte(t, c);
            return;
        }
    }
    else if (c >= 0xc2 && c <= 0xf4)
    {
        t->utf8_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        t->utf8_min = c < 0xe0 ? 128 : c < 0xf0 ? 2048 : 65536;
        t->utf8 = c & ((1u << (6 - t->utf8_left)) - 1);
        return;
    }
    else if (c >= 128)
        c = '?';
    if (c == 27)
    {
        t->state = 1;
        return;
    }
    if (t->state == 1)
    {
        t->state = 0;
        if (c == '[')
        {
            t->state = 2;
            t->count = t->private_mode = 0;
            for (unsigned i = 0; i < 8; i++)
                t->parameters[i] = 0;
        }
        else if (c == '7')
        {
            t->saved_row = t->row;
            t->saved_column = t->column;
        }
        else if (c == '8')
        {
            t->row = t->saved_row;
            t->column = t->saved_column;
            t->wrap = 0;
        }
        return;
    }
    if (t->state == 2)
    {
        if (c == '?')
        {
            t->private_mode = 1;
            return;
        }
        if (c >= '0' && c <= '9')
        {
            unsigned *p = &t->parameters[t->count];
            *p = *p < 1000 ? *p * 10 + c - '0' : 9999;
            return;
        }
        if (c == ';')
        {
            if (t->count < 7)
                t->count++;
            else
                t->state = 3;
            return;
        }
        if (c >= 0x40 && c <= 0x7e)
        {
            command(t, c);
            t->state = 0;
        }
        return;
    }
    if (t->state == 3)
    {
        if (c >= 0x40 && c <= 0x7e)
            t->state = 0;
        return;
    }
    if (c == '\r')
    {
        t->column = 0;
        t->wrap = 0;
        return;
    }
    if (c == '\n')
    {
        t->column = 0;
        t->wrap = 0;
        down(t);
        return;
    }
    if (c == 8)
    {
        t->wrap = 0;
        if (t->column)
            t->column--;
        return;
    }
    if (c == '\t')
    {
        unsigned spaces = 8 - (t->column % 8);
        while (spaces--)
            terminal_byte(t, ' ');
        return;
    }
    if (c < 32 || c == 127)
        return;
    if (t->wrap)
    {
        t->column = 0;
        down(t);
        t->wrap = 0;
    }
    cell_store(t, t->row * t->columns + t->column, t->attribute | c);
    if (++t->column == t->columns)
    {
        t->column = t->columns - 1;
        t->wrap = 1;
    }
}

/* Plain-text runs move existing rows once and redraw only surviving cells. ANSI sequences,
 * UTF-8 and control bytes are processed one byte at a time. Rendering uses cell operations
 * without a framebuffer copy or MMIO string operation. */
static unsigned plain_scroll(struct terminal *t, const unsigned char *bytes, unsigned n)
{
    if (n <= TERM_CONTROL_BATCH || t->state || t->utf8_left)
        return 0;
    if (n > TERM_PLAIN_BATCH)
        n = TERM_PLAIN_BATCH;
    unsigned row = t->row, column = t->column, wrap = t->wrap, count = 0;
    for (; count < n; count++)
    {
        unsigned c = bytes[count];
        if (c == '\n')
        {
            row++;
            column = wrap = 0;
        }
        else if (c >= 32 && c < 127)
        {
            if (wrap)
            {
                row++;
                column = wrap = 0;
            }
            if (++column == t->columns)
            {
                column--;
                wrap = 1;
            }
        }
        else
            break;
    }
    if (count <= TERM_CONTROL_BATCH || row < t->rows)
        return 0;
    unsigned shift = row - t->rows + 1, columns = t->columns, rows = t->rows;
    /* Remove the cursor overlay before copying the underlying cells. */
    if (t->watch_active && !t->watch_touched)
    {
        terminal_mmio_write(t->cells + t->watch_at, t->watch_cell);
        t->watch_touched = 1;
    }
    flush_erase(t);
    touch(t, 0, rows * columns);
    unsigned moved = shift < rows ? (rows - shift) * columns : 0;
    volatile uint32_t *to = t->cells, *from = to + (shift < rows ? shift : rows) * columns;
    unsigned left = moved;
    while (left >= 4)
    {
        terminal_mmio_write(to, terminal_mmio_read(from));
        terminal_mmio_write(to + 1, terminal_mmio_read(from + 1));
        terminal_mmio_write(to + 2, terminal_mmio_read(from + 2));
        terminal_mmio_write(to + 3, terminal_mmio_read(from + 3));
        to += 4;
        from += 4;
        left -= 4;
    }
    while (left--)
        terminal_mmio_write(to++, terminal_mmio_read(from++));
    t->erase_blank = t->attribute | ' ';
    blank_range(t, moved, rows * columns, t->erase_blank);
    unsigned logical = t->row, col = t->column, pending = t->wrap;
    for (unsigned i = 0; i < count; i++)
    {
        unsigned c = bytes[i];
        if (c == '\n')
        {
            logical++;
            col = pending = 0;
            continue;
        }
        if (pending)
        {
            logical++;
            col = pending = 0;
        }
        if (logical >= shift)
            terminal_mmio_write(t->cells + (logical - shift) * columns + col, t->attribute | c);
        if (++col == columns)
        {
            col--;
            pending = 1;
        }
    }
    t->row = row - shift;
    t->column = column;
    t->wrap = wrap;
    return count;
}

void terminal_write(struct terminal *t, const unsigned char *bytes, unsigned n)
{
    while (n)
    {
        unsigned done = plain_scroll(t, bytes, n);
        if (done)
        {
            bytes += done;
            n -= done;
            continue;
        }
        unsigned c = *bytes;
        if (!t->state && !t->utf8_left && !t->wrap && c >= 32 && c < 127)
        {
            unsigned count = 0, room = t->columns - t->column;
            unsigned at = t->row * t->columns + t->column, attribute = t->attribute;
            volatile uint32_t *p = t->cells + at;
            /* Consume each payload byte once: SDRAM has no data cache. */
            if (at > t->erase_first && at < t->erase_end)
            {
                blank_range(t, t->erase_first, at, t->erase_blank);
                t->erase_first = at;
            }
            do
            {
                terminal_mmio_write(p++, attribute | c);
                count++;
                if (count == n || count == room)
                    break;
                c = bytes[count];
            } while (c >= 32 && c < 127);
            cover_erase(t, at, count);
            touch(t, at, at + count);
            t->column += count;
            if (t->column == t->columns)
            {
                t->column--;
                t->wrap = 1;
            }
            bytes += count;
            n -= count;
        }
        else
        {
            terminal_byte(t, c);
            bytes++;
            n--;
        }
    }
}
