#include "lume/user_stack.h"
#include "lume/abi.h"

_Static_assert(LUME_ARG_BYTES <= UINT16_MAX && LUME_ENV_BYTES <= UINT16_MAX,
               "Argument and environment string sizes fit in uint16_t");

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

bool lume_stack_build_exec(const uint64_t *pages, unsigned argc, const char *const *argv,
                           unsigned envc, const char *const *envp, uint64_t entry, bool secure,
                           uint64_t *stack)
{
    if (secure)
        envc = 0;
    if (argc > LUME_ARG_MAX || envc > LUME_ENV_MAX || (argc && !argv) || (envc && !envp))
        return false;
    uint16_t sizes[LUME_ARG_MAX + LUME_ENV_MAX];
    size_t argument_bytes = 0, environment_bytes = 0;
    for (unsigned i = 0; i < argc; ++i)
    {
        size_t bytes;
        if (!string_size(argv[i], LUME_ARG_BYTES - argument_bytes, &bytes))
            return false;
        sizes[i] = (uint16_t)bytes;
        argument_bytes += bytes;
    }
    for (unsigned i = 0; i < envc; ++i)
    {
        size_t bytes;
        if (!string_size(envp[i], LUME_ENV_BYTES - environment_bytes, &bytes))
            return false;
        sizes[argc + i] = (uint16_t)bytes;
        environment_bytes += bytes;
    }
    size_t strings = LUME_STACK_PAGES * 4096u - argument_bytes - environment_bytes;
    const uintptr_t standard[] = {6, 4096, 9, entry};
    const uintptr_t secure_entry[] = {23, 1}; /* AT_SECURE */
    const uintptr_t terminator[] = {0, 0};
    size_t vectors = (argc + envc + 3u) * sizeof(uintptr_t);
    size_t auxiliary = sizeof(standard) + sizeof(terminator) + (secure ? sizeof(secure_entry) : 0);
    size_t position = (strings - vectors - auxiliary) & ~(size_t)15;
    size_t words = position;
    uintptr_t value = argc;
    write_bytes(pages, words, &value, sizeof(value));
    words += sizeof(value);
    for (unsigned group = 0; group < 2; ++group)
    {
        unsigned count = group ? envc : argc;
        const char *const *items = group ? envp : argv;
        for (unsigned i = 0; i < count; ++i)
        {
            value = LUME_USER_STACK + strings;
            write_bytes(pages, words, &value, sizeof(value));
            words += sizeof(value);
            size_t bytes = sizes[(group ? argc : 0) + i];
            write_bytes(pages, strings, items[i], bytes);
            strings += bytes;
        }
        value = 0;
        write_bytes(pages, words, &value, sizeof(value));
        words += sizeof(value);
    }
    write_bytes(pages, words, standard, sizeof(standard));
    words += sizeof(standard);
    if (secure)
    {
        write_bytes(pages, words, secure_entry, sizeof(secure_entry));
        words += sizeof(secure_entry);
    }
    write_bytes(pages, words, terminator, sizeof(terminator));
    *stack = LUME_USER_STACK + position;
    return true;
}

bool lume_stack_build(const uint64_t *pages, unsigned argc, const char *const *argv, unsigned envc,
                      const char *const *envp, uint64_t entry, uint64_t *stack)
{
    return lume_stack_build_exec(pages, argc, argv, envc, envp, entry, false, stack);
}
