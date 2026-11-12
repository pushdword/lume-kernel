/* SPDX-License-Identifier: BSD-3-Clause */
#define DEVICE_FIRST UINT64_C(0x9000000000)
#define DEVICE_END UINT64_C(0x10000000000)
#define DEVICE_DIRECTORY_LIMIT 16u
#define DEVICE_TABLE_LIMIT 64u
#define DEVICE_BYTES ((uint64_t)DEVICE_TABLE_LIMIT * BLOCK)

_Static_assert(DEVICE_FIRST >= LUME_THREAD_VM_LIMIT && DEVICE_FIRST >= LUME_HEAP_EXTENDED_LIMIT &&
                   DEVICE_FIRST >= LUME_LARGE_ELF_LIMIT && DEVICE_END <= LUME_VM_BASE &&
                   (DEVICE_FIRST >> 39) == 1 && ((DEVICE_END - 1) >> 39) == 1,
               "PCI device mappings must exclude application address ranges");

static _Alignas(4096) uint64_t device_pdpt[512];
static _Alignas(4096) uint64_t device_directories[DEVICE_DIRECTORY_LIMIT][512];
static _Alignas(4096) uint64_t device_pages[DEVICE_TABLE_LIMIT][512];
static _Alignas(4096) uint64_t task_device_pdpt[LUME_TASK_LIMIT][2][512];
static struct lume_range device_ram[8];
static unsigned device_ram_count, device_directory_count, device_table_count;
static bool device_initialized;

static void device_mapping_start(const struct lume_fdt *description)
{
    memset(device_pdpt, 0, sizeof(device_pdpt));
    memset(device_directories, 0, sizeof(device_directories));
    memset(device_pages, 0, sizeof(device_pages));
    memset(task_device_pdpt, 0, sizeof(task_device_pdpt));
    device_directory_count = device_table_count = 0;
    device_ram_count = description->ram_count;
    memcpy(device_ram, description->ram, device_ram_count * sizeof(device_ram[0]));
    root[1] = table(device_pdpt, false);
    device_initialized = true;
}

/* Device mappings remain wired while PCI resources can be rediscovered. */
bool lume_mmu_device_prepare(uint64_t address, uint64_t bytes)
{
    if (!bytes)
        return false;
    if (address >= UINT64_C(0xc0000000) && address < UINT64_C(0x100000000))
        return bytes <= UINT64_C(0x100000000) - address;
    if (!device_initialized || address < DEVICE_FIRST || address >= DEVICE_END ||
        bytes > DEVICE_END - address || bytes > DEVICE_BYTES)
        return false;
    uint64_t first = address & ~UINT64_C(4095);
    uint64_t end = (address + bytes + 4095) & ~UINT64_C(4095);
    if (end - first > DEVICE_BYTES)
        return false;
    for (unsigned i = 0; i < device_ram_count; ++i)
        if (device_ram[i].base < end && first < device_ram[i].base + device_ram[i].size)
            return false;
    unsigned needed_directories = 0, needed_tables = 0, previous = 512;
    for (uint64_t base = first & ~(BLOCK - 1); base < end; base += BLOCK)
    {
        unsigned index = (unsigned)((base >> 30) & 511);
        uint64_t value = device_pdpt[index];
        if (!value)
        {
            if (previous != index)
                ++needed_directories;
            ++needed_tables;
        }
        else
        {
            if ((value & (PRESENT | USER | HUGE)) != PRESENT)
                return false;
            const uint64_t *directory = (const void *)(uintptr_t)(value & ADDRESS);
            if (!directory[(base >> 21) & 511])
                ++needed_tables;
        }
        previous = index;
    }
    if (needed_directories > DEVICE_DIRECTORY_LIMIT - device_directory_count ||
        needed_tables > DEVICE_TABLE_LIMIT - device_table_count)
        return false;
    for (uint64_t base = first; base < end; base += 4096)
    {
        unsigned index = (unsigned)((base >> 30) & 511);
        if (!device_pdpt[index])
        {
            uint64_t value = table(device_directories[device_directory_count++], false);
            device_pdpt[index] = value;
            for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
                for (unsigned view = 0; view < 2; ++view)
                    if (task_root[task][view][0])
                        task_device_pdpt[task][view][index] = value;
        }
        uint64_t *directory = (void *)(uintptr_t)(device_pdpt[index] & ADDRESS);
        uint64_t *entry = directory + ((base >> 21) & 511);
        if (!*entry)
            *entry = table(device_pages[device_table_count++], false);
        uint64_t *pages = (void *)(uintptr_t)(*entry & ADDRESS);
        pages[(base >> 12) & 511] = base | PRESENT | WRITE | NX | UINT64_C(0x18);
    }
    lume_arch_flush_user_tlb();
    return true;
}

static bool device_mapping_present(uint64_t address)
{
    if (address >= UINT64_C(0xc0000000) && address < UINT64_C(0x100000000))
        return true;
    if (!device_initialized || address < DEVICE_FIRST || address >= DEVICE_END)
        return false;
    uint64_t value = device_pdpt[(address >> 30) & 511];
    if (!value)
        return false;
    const uint64_t *directory = (const void *)(uintptr_t)(value & ADDRESS);
    value = directory[(address >> 21) & 511];
    if (!value)
        return false;
    const uint64_t *pages = (const void *)(uintptr_t)(value & ADDRESS);
    value = pages[(address >> 12) & 511];
    return value == ((address & ~UINT64_C(4095)) | PRESENT | WRITE | NX | UINT64_C(0x18));
}
