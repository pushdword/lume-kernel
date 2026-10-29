#include "../../../drivers/display/rp1_dsi/display.c"

struct display_test_result
{
    unsigned failure, checks, cases, batches, polls;
};

struct display_test_result lume_display_test_result;
static uint32_t test_pixels[WIDTH * HEIGHT], reference_cells[65 * 24];
static struct terminal reference;
static void (*volatile write_display)(const char *, size_t) = lume_display_write;

__attribute__((noinline)) void lume_display_test_finish(void)
{
    for (;;)
        __asm__ volatile("wfe");
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++lume_display_test_result.checks;                                                         \
        if (!(condition))                                                                          \
        {                                                                                          \
            lume_display_test_result.failure = __LINE__;                                           \
            lume_display_test_finish();                                                            \
        }                                                                                          \
    } while (0)

_Noreturn void lume_fail(const char *why)
{
    (void)why;
    lume_display_test_result.failure = __LINE__;
    lume_display_test_finish();
    for (;;)
    {
    }
}

void lume_uart_poll_input(void)
{
    ++lume_display_test_result.polls;
}

static void reset(void)
{
    enabled = true;
    fb = test_pixels;
    terminal_start(&terminal, cells, 65, 24);
    terminal_start(&reference, reference_cells, 65, 24);
    for (unsigned i = 0; i < 65 * 24; ++i)
        shown[i] = cells[i];
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
        test_pixels[i] = colors[0];
}

static void compare(void)
{
    CHECK(terminal.row == reference.row && terminal.column == reference.column);
    CHECK(terminal.saved_row == reference.saved_row &&
          terminal.saved_column == reference.saved_column);
    CHECK(terminal.state == reference.state && terminal.count == reference.count);
    CHECK(terminal.private_mode == reference.private_mode && terminal.wrap == reference.wrap);
    CHECK(terminal.foreground == reference.foreground &&
          terminal.background == reference.background && terminal.inverse == reference.inverse &&
          terminal.attribute == reference.attribute);
    CHECK(terminal.utf8 == reference.utf8 && terminal.utf8_left == reference.utf8_left &&
          terminal.utf8_min == reference.utf8_min);
    CHECK(terminal.cursor_hidden == reference.cursor_hidden && !terminal.batch &&
          terminal.erase_first == terminal.erase_end);
    unsigned cursor = reference.row * 65 + reference.column;
    for (unsigned i = 0; i < 65 * 24; ++i)
    {
        CHECK(cells[i] == reference_cells[i]);
        CHECK(shown[i] ==
              (reference_cells[i] | (!reference.cursor_hidden && i == cursor ? 0x10000u : 0)));
    }
}

static void process(const char *data, size_t bytes, unsigned chunk)
{
    while (bytes)
    {
        size_t count = bytes < chunk ? bytes : chunk;
        for (size_t i = 0; i < count; ++i)
        {
            terminal_begin(&reference);
            terminal_byte(&reference, (unsigned char)data[i]);
            terminal_end(&reference);
        }
        lume_display_test_result.batches +=
            (unsigned)((count + LUME_DISPLAY_BATCH - 1) / LUME_DISPLAY_BATCH);
        unsigned before = lume_display_test_result.polls;
        write_display(data, count);
        CHECK(lume_display_test_result.polls >= before + 2);
        compare();
        data += count;
        bytes -= count;
    }
    ++lume_display_test_result.cases;
}

static uint64_t pixel_hash(void)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
        hash = (hash ^ test_pixels[i]) * UINT64_C(1099511628211);
    return hash;
}

static void compare_pixels(void)
{
    uint64_t actual = pixel_hash();
    unsigned cursor = reference.row * 65 + reference.column;
    for (unsigned i = 0; i < 65 * 24; ++i)
        render_cell(reference_cells[i] | (!reference.cursor_hidden && i == cursor ? 0x10000u : 0),
                    i);
    CHECK(pixel_hash() == actual);
}

__attribute__((section(".text.boot"))) void _start(void)
{
    static const char controls[] = "abc\tdef\b!\rPT: ol\xc3\xa1 \xc3\xa7 \xe2\x82\xac\n"
                                   "\033[31;44mred\033[0m\0337\033[3;9H@\0338\033[?25l\033[?25h"
                                   "\033[2K\033[1J\033[2J\033[Hdone\n"
                                   "\033[2;5H\033[s\033[4C?\033[u!\033[1K"
                                   "\xf0\x9f\x98\x80\xc3Z\x80\033[1;2;3;4;5;6;7;8;9mend";
    static const unsigned chunks[] = {1, 2, 3, 7, 31, 127, 256, 4096};
    reset();
    unsigned before = lume_display_test_result.polls;
    write_display(NULL, 0);
    CHECK(lume_display_test_result.polls == before);
    enabled = false;
    write_display(NULL, 12);
    CHECK(lume_display_test_result.polls == before && terminal.column == 0);
    for (unsigned i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i)
    {
        reset();
        process(controls, sizeof(controls) - 1, chunks[i]);
        compare_pixels();
    }
    static char text[4097];
    for (unsigned i = 0; i < sizeof(text); ++i)
        text[i] = i % 47 == 46 ? '\n' : (char)('a' + i % 26);
    reset();
    process(text, sizeof(text), sizeof(text));
    compare_pixels();
    static const char overwrite[] = "\033[?25l\033[2J\033[Hsame\rsame\033[?25h";
    process(overwrite, sizeof(overwrite) - 1, 256);
    compare_pixels();
    static const char binary[] = {'a', 0, 'b', (char)0xc3, (char)0xa1, '\n'};
    process(binary, sizeof(binary), 2);
    compare_pixels();
    lume_display_test_finish();
}
