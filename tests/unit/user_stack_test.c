#include "lume/user_stack.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char memory[LUME_STACK_PAGES][4096];
static uint64_t pages[LUME_STACK_PAGES];

static unsigned char byte(uint64_t address)
{
    assert(address >= LUME_USER_STACK && address < LUME_USER_LIMIT);
    size_t offset = (size_t)(address - LUME_USER_STACK);
    return ((unsigned char *)(uintptr_t)pages[offset / 4096])[offset % 4096];
}

static uint64_t word(uint64_t address)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)byte(address + i) << (8 * i);
    return value;
}

static void text(uint64_t address, const char *expected)
{
    do
    {
        assert(byte(address++) == (unsigned char)*expected);
    } while (*expected++);
}

int main(void)
{
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        pages[i] = (uintptr_t)memory[LUME_STACK_PAGES - i - 1];
    const char *arguments[LUME_ARG_MAX], *environment[LUME_ENV_MAX];
    char option[60], setting[120];
    memset(option, 'a', sizeof(option) - 1);
    option[sizeof(option) - 1] = 0;
    memset(setting, 'b', sizeof(setting) - 1);
    setting[sizeof(setting) - 1] = 0;
    for (unsigned i = 0; i < LUME_ARG_MAX; ++i)
        arguments[i] = option;
    for (unsigned i = 0; i < LUME_ENV_MAX; ++i)
        environment[i] = setting;
    uint64_t stack;
    assert(lume_stack_build(pages, LUME_ARG_MAX, arguments, LUME_ENV_MAX, environment, 0x40040000,
                            &stack));
    assert(!(stack & 15));
    assert(word(stack) == LUME_ARG_MAX);
    uint64_t position = stack + 8;
    for (unsigned i = 0; i < LUME_ARG_MAX; ++i, position += 8)
        text(word(position), option);
    assert(!word(position));
    position += 8;
    for (unsigned i = 0; i < LUME_ENV_MAX; ++i, position += 8)
        text(word(position), setting);
    assert(!word(position));
    position += 8;
    assert(word(position) == 6 && word(position + 8) == 4096);
    assert(word(position + 16) == 9 && word(position + 24) == 0x40040000);
    assert(!word(position + 32) && !word(position + 40));
    assert(!lume_stack_build(pages, LUME_ARG_MAX + 1, arguments, 0, 0, 1, &stack));
    assert(!lume_stack_build(pages, 1, 0, 0, 0, 1, &stack));
    static char oversized[LUME_ARG_BYTES + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    arguments[0] = oversized;
    assert(!lume_stack_build(pages, 1, arguments, 0, 0, 1, &stack));
    assert(lume_stack_build(pages, 0, 0, 0, 0, 1, &stack));
    assert(!word(stack) && !word(stack + 8) && !word(stack + 16));
    puts("PASS_USER_STACK arguments/environment/auxiliary/alignment/discontiguous pages/bounds");
}
