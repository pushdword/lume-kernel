#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lume/platform_driver.h"
#include "lume/storage.h"
#define LUME_Q35_H
#define LUME_COUNTER_H
#define LUME_ARCH_CPU_H
static uint16_t identity[256], sector_data[256];
static uint8_t registers[8];
static unsigned phase, cursor, commands, flushes, lba_count;
static uint32_t lbas[32];
static bool busy;
static uint64_t ticks;
static const struct lume_platform_sd_ops *registered;

uint64_t lume_counter_read_relaxed(void)
{
    ticks += 100000;
    return ticks;
}

void lume_cpu_relax(void)
{
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

uint8_t lume_in8(uint16_t port)
{
    if (port == 0x3f6)
        return busy ? 0x80 : phase ? 0x48 : 0x40;
    assert(port >= 0x1f0 && port <= 0x1f7);
    return registers[port - 0x1f0];
}

uint16_t lume_in16(uint16_t port)
{
    assert(port == 0x1f0 && (phase == 1 || phase == 2) && cursor < 256);
    uint16_t value = phase == 1 ? identity[cursor] : sector_data[cursor];
    if (++cursor == 256)
        phase = 0;
    return value;
}

void lume_out8(uint16_t port, uint8_t value)
{
    if (port == 0x3f6)
    {
        assert(value == 2);
        return;
    }
    assert(port >= 0x1f0 && port <= 0x1f7);
    registers[port - 0x1f0] = value;
    if (port != 0x1f7)
        return;
    if (value == 0x20 || value == 0x30)
    {
        assert(lba_count < 32);
        lbas[lba_count++] = registers[3] | (uint32_t)registers[4] << 8 |
                            (uint32_t)registers[5] << 16 | (uint32_t)(registers[6] & 15) << 24;
    }
    ++commands;
    cursor = 0;
    if (value == 0xec)
        phase = 1;
    else if (value == 0x20)
        phase = 2;
    else if (value == 0x30)
        phase = 3;
    else
    {
        assert(value == 0xe7);
        ++flushes;
        phase = 0;
    }
}

void lume_out16(uint16_t port, uint16_t value)
{
    assert(port == 0x1f0 && phase == 3 && cursor < 256);
    sector_data[cursor] = value;
    if (++cursor == 256)
        phase = 0;
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    assert(kind == LUME_PLATFORM_SD && bytes == sizeof(*registered));
    for (size_t offset = 0; offset < bytes; offset += sizeof(uintptr_t))
    {
        uintptr_t callback;
        memcpy(&callback, (const unsigned char *)ops + offset, sizeof(callback));
        assert(callback);
    }
    registered = ops;
    return 0;
}

#include "../../../drivers/storage/ata_pio/ata.c"

int main(void)
{
    identity[49] = 0x200;
    identity[82] = 0x20;
    identity[83] = 0x5000;
    uint32_t total = 33554304;
    identity[60] = (uint16_t)total;
    identity[61] = (uint16_t)(total >> 16);
    assert(!lume_pc_ide_register() && registered && registered->lume_sd_sectors() == total);
    unsigned char input[512], output[512];
    for (unsigned i = 0; i < 512; ++i)
        input[i] = (unsigned char)(i * 37 + 19);
    unsigned before = commands;
    assert(!registered->lume_sd_write(LUME_PC_DATA_FIRST, input) && commands == before);
    assert(!registered->lume_sd_allow_volume(2048, total - 2048));
    assert(!registered->lume_sd_allow_volume(LUME_PC_DATA_FIRST, total));
    assert(registered->lume_sd_allow_volume(LUME_PC_DATA_FIRST, total - LUME_PC_DATA_FIRST));
    assert(!registered->lume_sd_allow_boot_volume(1, 128));
    assert(!registered->lume_sd_write(0, input) && commands == before);
    assert(!registered->lume_sd_write(total, input) && commands == before);
    assert(registered->lume_sd_write(LUME_PC_DATA_FIRST, input) && flushes == 1);
    assert(registered->lume_sd_read(LUME_PC_DATA_FIRST, output));
    assert(!memcmp(input, output, sizeof(input)));
    identity[83] = 0x4000;
    assert(lume_pc_ide_register() == -5);
    identity[82] = 0;
    allowed_count = 0;
    assert(!lume_pc_ide_register());
    assert(registered->lume_sd_allow_volume(LUME_PC_DATA_FIRST, total - LUME_PC_DATA_FIRST));
    assert(registered->lume_sd_write(LUME_PC_DATA_FIRST, input) && flushes == 1);
    before = commands;
    assert(!registered->lume_sd_read(total, output) && commands == before);
#if defined(LUME_SD_MAX_SECTORS)
    unsigned char pair[1024], received_pair[1024];
    memcpy(pair, input, 512);
    memcpy(pair + 512, input, 512);
    uint64_t read_count = reads, write_count = writes;
    unsigned first_lba = lba_count;
    assert(registered->lume_sd_write_blocks(LUME_PC_DATA_FIRST, 2, pair));
    assert(registered->lume_sd_read_blocks(LUME_PC_DATA_FIRST, 2, received_pair));
    for (unsigned i = 0; i < 4; ++i)
        assert(lbas[first_lba + i] == LUME_PC_DATA_FIRST + (i & 1));
    assert(!memcmp(pair, received_pair, sizeof(pair)) && reads == read_count + 2 &&
           writes == write_count + 2);
    before = commands;
    assert(!registered->lume_sd_read_blocks(0, 0, received_pair));
    assert(!registered->lume_sd_read_blocks(0, LUME_SD_MAX_SECTORS + 1, received_pair));
    assert(!registered->lume_sd_read_blocks(total - 1, 2, received_pair));
    assert(!registered->lume_sd_write_blocks(total - 1, 2, pair));
    assert(!registered->lume_sd_write_blocks(LUME_PC_DATA_FIRST - 1, 2, pair));
    assert(!registered->lume_sd_write_blocks(LUME_PC_DATA_FIRST, 1, NULL));
    assert(commands == before);
#endif
    busy = true;
    assert(!registered->lume_sd_read(LUME_PC_DATA_FIRST, output));
    assert(!registered->lume_sd_write(LUME_PC_DATA_FIRST, input));
    assert(commands == before && failed && failures == 1);
    puts("PASS ATA PIO, volume bounds, cache capabilities, flush and bounded timeout");
    return 0;
}
