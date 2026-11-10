#define main storage_handles_original_main
#include "storage_handles_test.c"
#undef main

static bool seek_maps(void)
{
    const struct lume_storage_ops *ops = format(false);
    static unsigned char input[1024 * 1024];
    unsigned char output[4096], changed[512];
    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)(i * 29u + i / 251u);
    memset(changed, 0x73, sizeof(changed));
    void *writer, *reader;
    T(ops && ops->create("/data/seek.bin", false) == 0);
    T(ops->open("/data/seek.bin", LUME_O_RDWR, &writer) == 0);
    T(ops->write_handle(writer, 0, input, sizeof(input)) == sizeof(input));
    T(ops->sync_handle(writer) == 0 && ops->close(writer) == 0);
    T(ops->open("/data/seek.bin", LUME_O_RDONLY, &reader) == 0);
    T(ops->read_handle(reader, sizeof(input) - sizeof(output), output, sizeof(output)) ==
      sizeof(output));
    T(!memcmp(output, input + sizeof(input) - sizeof(output), sizeof(output)));
    unsigned before = reads;
    T(ops->read_handle(reader, sizeof(input) - 2 * sizeof(output), output, sizeof(output)) ==
      sizeof(output));
    T(!memcmp(output, input + sizeof(input) - 2 * sizeof(output), sizeof(output)));
    T(reads - before <= 10);
    T(ops->open("/data/seek.bin", LUME_O_RDWR, &writer) == 0);
    T(ops->write_handle(writer, sizeof(input) - sizeof(changed), changed, sizeof(changed)) ==
      sizeof(changed));
    T(ops->read_handle(reader, sizeof(input) - sizeof(changed), output, sizeof(changed)) ==
      sizeof(changed));
    T(!memcmp(output, changed, sizeof(changed)));
    T(ops->resize_handle(writer, sizeof(input) + sizeof(output)) == 0);
    T(ops->read_handle(reader, sizeof(input), output, sizeof(output)) == sizeof(output));
    for (unsigned i = 0; i < sizeof(output); ++i)
        T(!output[i]);
    T(ops->sync_handle(writer) == 0 && ops->close(writer) == 0 && ops->close(reader) == 0);

    ops = format(false);
    void *gap;
    T(ops && ops->create("/data/fragments.bin", false) == 0 &&
      ops->create("/data/gaps.bin", false) == 0);
    T(ops->open("/data/fragments.bin", LUME_O_RDWR, &writer) == 0);
    T(ops->open("/data/gaps.bin", LUME_O_RDWR, &gap) == 0);
    for (unsigned i = 0; i < 16; ++i)
    {
        T(ops->write_handle(writer, i * 65536u, input + i * 65536u, 65536) == 65536);
        T(ops->write_handle(gap, i * 512u, changed, 512) == 512);
    }
    T(ops->sync_handle(writer) == 0 && ops->sync_handle(gap) == 0);
    T(ops->close(writer) == 0 && ops->close(gap) == 0);
    T(ops->open("/data/fragments.bin", LUME_O_RDONLY, &reader) == 0);
    T(ops->read_handle(reader, sizeof(input) - sizeof(output), output, sizeof(output)) ==
      sizeof(output));
    T(!memcmp(output, input + sizeof(input) - sizeof(output), sizeof(output)));
    T(ops->read_handle(reader, 0, output, sizeof(output)) == sizeof(output));
    T(!memcmp(output, input, sizeof(output)) && ops->close(reader) == 0);
    return true;
}

int main(void)
{
    if (!seek_maps())
        return 1;
    printf("PASS_STORAGE_SEEK checks=%u reads=%u writes=%u\n", checks, reads, writes);
    return 0;
}
