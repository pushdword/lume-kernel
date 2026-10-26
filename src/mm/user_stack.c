#include "lume/user_stack.h"
#include "lume/abi.h"

static bool string_size(const char *text, size_t available, size_t *bytes)
{
    if (!text)
        return false;
    for (size_t i = 0; i < available; ++i)
        if (!text[i])
        {
            *bytes = i + 1;
            return true;
        }
    return false;
}

static void write_bytes(const uint64_t *pages, size_t offset, const void *data, size_t bytes)
{
    const unsigned char *in = data;
    for (size_t i = 0; i < bytes; ++i)
    {
        size_t position = offset + i;
        unsigned char *out = (void *)(uintptr_t)pages[position / 4096];
        out[position % 4096] = in[i];
    }
}

bool lume_stack_build(const uint64_t *pages, unsigned argc, const char *const *argv, unsigned envc,
                      const char *const *envp, uint64_t entry, uint64_t *stack)
{
    if (argc > LUME_ARG_MAX || envc > LUME_ENV_MAX || (argc && !argv) || (envc && !envp))
        return false;
    size_t sizes[LUME_ARG_MAX + LUME_ENV_MAX], argument_bytes = 0, environment_bytes = 0;
    for (unsigned i = 0; i < argc; ++i)
    {
        if (!string_size(argv[i], LUME_ARG_BYTES - argument_bytes, &sizes[i]))
            return false;
        argument_bytes += sizes[i];
    }
    for (unsigned i = 0; i < envc; ++i)
    {
        if (!string_size(envp[i], LUME_ENV_BYTES - environment_bytes, &sizes[argc + i]))
            return false;
        environment_bytes += sizes[argc + i];
    }
    size_t strings = LUME_STACK_PAGES * 4096u - argument_bytes - environment_bytes;
    size_t position = (strings - (argc + envc + 9u) * 8u) & ~(size_t)15;
    size_t words = position;
    uint64_t value = argc;
    write_bytes(pages, words, &value, 8);
    words += 8;
    for (unsigned group = 0; group < 2; ++group)
    {
        unsigned count = group ? envc : argc;
        const char *const *items = group ? envp : argv;
        for (unsigned i = 0; i < count; ++i)
        {
            value = LUME_USER_STACK + strings;
            write_bytes(pages, words, &value, 8);
            words += 8;
            size_t bytes = sizes[(group ? argc : 0) + i];
            write_bytes(pages, strings, items[i], bytes);
            strings += bytes;
        }
        value = 0;
        write_bytes(pages, words, &value, 8);
        words += 8;
    }
    const uint64_t auxiliary[] = {6, 4096, 9, entry, 0, 0};
    write_bytes(pages, words, auxiliary, sizeof(auxiliary));
    *stack = LUME_USER_STACK + position;
    return true;
}
