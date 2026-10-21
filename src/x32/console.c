#include "core.h"
#include "terminal.h"
#ifndef LUME_CONSOLE_HOST_TEST
#include "uart_queue.h"
#else
static int uart_queue_active(void)
{
    return 0;
}

static int uart_queue_read(void)
{
    return -11;
}
#endif
static unsigned char kernel_logs[MAX_PROCESSES + 2][4096];
static unsigned kernel_log_sizes[MAX_PROCESSES + 2];

static unsigned log_core(void)
{
#ifdef LUME_CONSOLE_HOST_TEST
    return 0;
#else
    unsigned core = core_id();
    return current[core] >= 0 ? (unsigned)current[core] : MAX_PROCESSES + core;
#endif
}

unsigned console_kernel_size(void)
{
    return kernel_log_sizes[log_core()];
}

unsigned console_kernel_take(unsigned char *out, unsigned capacity)
{
    unsigned core = log_core();
    unsigned char *kernel_log = kernel_logs[core];
    unsigned kernel_log_size = kernel_log_sizes[core];
    if (kernel_log_size > capacity)
        panic("kernel diagnostic buffer capacity");
    unsigned n = kernel_log_size;
    for (unsigned i = 0; i < n; i++)
        out[i] = kernel_log[i];
    kernel_log_sizes[core] = 0;
    return n;
}

static unsigned video, extended, keyboard, last_present, cursor_visible, cursor_at, cursor_cell,
    focus;
static unsigned position[2], available[2];
static struct terminal screen[2];
/* queue_lock protects input processing. Input processing does not access an active renderer. */
static unsigned render_active, input_focus, cached_geometry[2];
static struct keymap layout[2];
static unsigned char pending[2][16];
static volatile uint32_t *const cells = (volatile uint32_t *)0xf0010100u;
static volatile uint32_t *const kbd = (volatile uint32_t *)0xf0005000u;

static void cursor_hide(void)
{
    if (cursor_visible)
    {
        terminal_mmio_write(cells + cursor_at, cursor_cell);
        cursor_visible = 0;
    }
}

static void cursor_show(void)
{
    struct terminal *t = &screen[focus];
    if (t->cursor_hidden)
        return;
    cursor_at = (unsigned)(t->cells - cells) + t->row * t->columns + t->column;
    cursor_cell = terminal_cell(t, t->row * t->columns + t->column);
    terminal_mmio_write(cells + cursor_at, cursor_cell ^ 0x7700u);
    cursor_visible = 1;
}

static void begin_write(unsigned tty)
{
    struct terminal *t = &screen[tty];
    t->watch_active = tty == focus && cursor_visible;
    if (t->watch_active)
    {
        t->watch_at = cursor_at - (unsigned)(t->cells - cells);
        t->watch_cell = cursor_cell;
    }
    terminal_begin(t);
}

static void end_write(unsigned tty)
{
    struct terminal *t = &screen[tty];
    terminal_end(t);
    if (tty == focus)
    {
        unsigned next = (unsigned)(t->cells - cells) + t->row * t->columns + t->column;
        if (cursor_visible && (t->watch_touched || cursor_at != next || t->cursor_hidden))
        {
            if (t->watch_touched)
                cursor_visible = 0;
            else
                cursor_hide();
        }
        t->watch_active = 0;
        if (!cursor_visible)
            cursor_show();
    }
    t->watch_active = 0;
}

void console_render_begin(void)
{
    render_active = 1;
}

void console_render_end(void)
{
    render_active = 0;
    for (unsigned i = 0; i < 2; i++)
        cached_geometry[i] = (screen[i].rows << 16) | screen[i].columns;
    if (video && focus != input_focus)
    {
        cursor_hide();
        focus = input_focus;
        cursor_show();
    }
}

static void panes_start(unsigned columns, unsigned rows)
{
    unsigned first = rows / 2;
    terminal_start(&screen[0], cells, columns, first);
    terminal_start(&screen[1], cells + columns * first, columns, rows - first);
}

void console_start(void)
{
    unsigned id = *(volatile uint32_t *)0xf0010000u;
    video = id == 0x31545854u;
    // This board's fixed HWDT record6 advertises the extended aperture.
    // Never probe its new register on the installed16KiB legacy controller:
    // an unsupported MMIO read is a CPU bus fault, not a capability result.
    extended = video && *(volatile uint32_t *)0xf00020d0u == 32768u;
    if (extended)
        extended = *(volatile uint32_t *)0xf0010018u == 0x32545854u;
    if (video)
    {
        *(volatile uint32_t *)0xf0010010u = extended ? 3 : 1;
        unsigned geometry = *(volatile uint32_t *)0xf0010008u;
        unsigned columns = geometry & 65535, rows = geometry >> 16;
        if (!columns || columns > TERM_COLUMNS || rows < 2 || rows > TERM_ROWS)
        {
            columns = 100;
            rows = 37;
        }
        panes_start(columns, rows);
        console_render_end();
    }
    keyboard = kbd[0] == 0x3144424bu;
    if (keyboard)
        kbd[3] = 3;
}

static void uart(unsigned c)
{
    unsigned budget = 1000000;
    while (!(*(volatile uint32_t *)0xf0000004u & 1u))
    {
        if (!--budget)
            panic("legacy UART not ready");
    }
    *(volatile unsigned char *)0xf0000000u = (unsigned char)c;
}

static void emit(unsigned tty, unsigned c)
{
    if (!tty)
    {
        if (c == '\n')
            uart('\r');
        uart(c);
    }
    if (video)
        terminal_byte(&screen[tty], c);
}

void kputc(unsigned c)
{
    if (uart_queue_active())
    {
        unsigned core = log_core(), n = kernel_log_sizes[core];
        if (n == sizeof kernel_logs[core])
            panic("kernel diagnostic buffer full");
        kernel_logs[core][n] = (unsigned char)c;
        kernel_log_sizes[core] = n + 1;
        return;
    }
    if (video)
        begin_write(0);
    emit(0, c);
    if (video)
        end_write(0);
}

void console_write_tty(unsigned tty, const unsigned char *bytes, unsigned n)
{
    if (tty > 1)
        return;
    if (video)
        begin_write(tty);
    /* Both panels consume logical text runs. Only the UART expands carriage returns. TTY0
     * publishes the visual batch before draining its serial output. */
    if (video)
        terminal_write(&screen[tty], bytes, n);
    if (video)
        end_write(tty);
    if (!tty && !uart_queue_active())
        for (unsigned i = 0; i < n; i++)
        {
            if (bytes[i] == '\n')
                uart('\r');
            uart(bytes[i]);
        }
}

void console_write(const unsigned char *bytes, unsigned n)
{
    console_write_tty(0, bytes, n);
}

static void append_input(unsigned tty, const unsigned char *bytes, unsigned n)
{
    unsigned left = available[tty] - position[tty];
    for (unsigned i = 0; i < left; i++)
        pending[tty][i] = pending[tty][position[tty] + i];
    position[tty] = 0;
    available[tty] = left;
    if (n <= sizeof pending[tty] - left)
    {
        for (unsigned i = 0; i < n; i++)
            pending[tty][available[tty]++] = bytes[i];
    }
}

static void fill_input(void)
{
    if (available[0] - position[0] < sizeof pending[0])
    {
        if (uart_queue_active())
        {
            for (unsigned n = 0;
                 n < sizeof pending[0] && available[0] - position[0] < sizeof pending[0]; n++)
            {
                int c = uart_queue_read();
                if (c < 0)
                    break;
                unsigned char byte = (unsigned char)c;
                append_input(0, &byte, 1);
            }
        }
        else if (*(volatile uint32_t *)0xf0000004u & 2u)
        {
            unsigned char c = *(volatile unsigned char *)0xf0000000u;
            append_input(0, &c, 1);
        }
    }
    if (!keyboard)
        return;
    unsigned status = kbd[1], present = !!(status & 2u);
    if (!present)
    {
        if (last_present)
        {
            layout[0] = layout[1] = (struct keymap){0};
            kbd[3] = 3;
        }
        last_present = 0;
        return;
    }
    last_present = 1;
    if (status & 8u)
    {
        kbd[3] = 3;
        layout[0].dead = layout[1].dead = 0;
        return;
    }
    for (unsigned i = 0; i < 8 && (kbd[1] & 4u); i++)
    {
        unsigned event = kbd[2], usage = event & 255, mods = (event >> 8) & 255;
        if (video && (mods & 4u) && (usage == 0x3a || usage == 0x3b))
        {
            input_focus = usage - 0x3a;
            if (!render_active)
            {
                cursor_hide();
                focus = input_focus;
                cursor_show();
            }
            continue;
        }
        unsigned char bytes[16];
        unsigned n = keymap_pt(&layout[input_focus], usage, mods, bytes);
        if (n)
            append_input(input_focus, bytes, n);
    }
}

int console_ready_tty(unsigned tty)
{
    if (tty > 1)
        return 0;
    fill_input();
    return position[tty] < available[tty];
}

int console_getchar_tty(unsigned tty)
{
    return console_ready_tty(tty) ? pending[tty][position[tty]++] : -11;
}

int console_ready(void)
{
    return console_ready_tty(0);
}

int console_getchar(void)
{
    return console_getchar_tty(0);
}

unsigned console_geometry_tty(unsigned tty)
{
    return video && tty < 2 ? cached_geometry[tty] : (24u << 16) | 80u;
}

unsigned console_geometry(void)
{
    return console_geometry_tty(0);
}

unsigned console_tty_supported(unsigned tty)
{
    return tty == 0 || (tty == 1 && video);
}

int console_video(unsigned action, unsigned value, const unsigned char *font)
{
    if (action == 0)
        return extended ? 3
               : video  ? 1
                        : 0; /* These display modes are supported without EDID discovery. */
    if (action >= 3 && action <= 5)
    {
        if (!video)
            return -95;
        return (int)*(volatile uint32_t *)(uintptr_t)(action == 3   ? 0xf0010014u
                                                      : action == 4 ? 0xf001000cu
                                                                    : 0xf0010004u);
    }
    if (!extended)
        return -95;
    if (action == 1)
    {
        if (value > 1)
            return -22;
        cursor_hide();
        *(volatile uint32_t *)0xf0010010u = 1 | (value << 1);
        panes_start(value ? 128 : 100, value ? 48 : 37);
        cursor_show();
        return 0;
    }
    if (action == 2)
    {
        volatile uint32_t *ram = (volatile uint32_t *)0xf0017000u;
        for (unsigned i = 0; i < 1024; i++)
        {
            const unsigned char *p = font + i * 4;
            ram[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[3] << 24);
        }
        return 0;
    }
    return -22;
}

void kputs(const char *s)
{
    if (uart_queue_active())
    {
        while (*s)
            kputc((unsigned char)*s++);
        return;
    }
    if (video)
        begin_write(0);
    while (*s)
        emit(0, (unsigned char)*s++);
    if (video)
        end_write(0);
}

void knumber(uint32_t n)
{
    char b[10];
    unsigned i = 0;
    do
    {
        b[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i)
        kputc((unsigned)b[--i]);
}

void khex(uint32_t n)
{
    for (unsigned s = 32; s;)
    {
        s -= 4;
        unsigned d = (n >> s) & 15;
        kputc(d < 10 ? '0' + d : 'A' + d - 10);
    }
}
#ifndef LUME_CONSOLE_HOST_TEST
_Noreturn void panic(const char *s)
{
    __asm__ volatile("cli" ::: "memory", "cc");
    uart_queue_emergency("\nLUME KERNEL PANIC: ");
    uart_queue_emergency(s);
    uart_queue_emergency("\n");
    for (;;)
        __asm__ volatile("halt" ::: "memory", "cc");
}
#endif
