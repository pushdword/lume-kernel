#include <stdint.h>

extern unsigned lume_fixture_export(unsigned);
static unsigned counter = 7;
static unsigned char storage[512];

static unsigned run(unsigned value)
{
    storage[value & 511] = (unsigned char)value;
    counter += value;
    return lume_fixture_export(counter);
}

const struct
{
    unsigned (*run)(unsigned);
    unsigned *counter;
    unsigned char *storage;
    uint64_t marker;
} lume_module_descriptor = {run, &counter, storage, UINT64_C(0x123456789abcdef0)};
