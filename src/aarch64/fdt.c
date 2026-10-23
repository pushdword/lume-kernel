#include "lume/early.h"

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static size_t length(const unsigned char *p, size_t limit)
{
    size_t n = 0;
    while (n < limit && p[n])
        ++n;
    return n;
}

static bool equals(const unsigned char *p, size_t n, const char *s)
{
    size_t i = 0;
    while (s[i])
    {
        if (i >= n || p[i] != (unsigned char)s[i])
            return false;
        ++i;
    }
    return i < n && p[i] == 0;
}

static bool contains(const unsigned char *p, size_t n, const char *s)
{
    while (n)
    {
        size_t len = length(p, n);
        if (len == n)
            return false;
        if (equals(p, n, s))
            return true;
        p += len + 1;
        n -= len + 1;
    }
    return false;
}

static bool span(uint32_t start, uint32_t count, uint32_t size)
{
    return start <= size && count <= size - start;
}

static uint64_t be64(const unsigned char *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static bool ranges(struct lume_range *out, unsigned *count, unsigned limit, const unsigned char *p,
                   uint32_t bytes)
{
    if (bytes % 16)
        return false;
    for (uint32_t i = 0; i < bytes; i += 16)
    {
        uint64_t base = be64(p + i), size = be64(p + i + 8);
        if (!size)
            continue;
        if (base > UINT64_MAX - size || *count == limit)
            return false;
        out[(*count)++] = (struct lume_range){base, size};
    }
    return true;
}

bool lume_fdt_read(const void *blob, size_t available, struct lume_fdt *out)
{
    const unsigned char *b = blob;
    *out = (struct lume_fdt){0};
    if (available < 40 || be32(b) != 0xd00dfeed)
        return false;
    uint32_t total = be32(b + 4), pos = be32(b + 8), strings = be32(b + 12);
    uint32_t strings_size = be32(b + 32), structure_size = be32(b + 36);
    if (total < 40 || total > available || total > 0x200000 || be32(b + 20) < 17 ||
        be32(b + 24) > 17 || pos < 40 || strings < 40 || (pos & 3) ||
        !span(pos, structure_size, total) || !span(strings, strings_size, total))
        return false;
    uint32_t end = pos + structure_size;
    uint32_t reserve = be32(b + 16);
    if (reserve < 40 || (reserve & 7))
        return false;
    for (;;)
    {
        if (!span(reserve, 16, total))
            return false;
        if (!be64(b + reserve) && !be64(b + reserve + 8))
            break;
        if (!ranges(out->reserved, &out->reserved_count, 32, b + reserve, 16))
            return false;
        reserve += 16;
    }
    unsigned depth = 0;
    bool root_seen = false, root_closed = false, in_psci = false;
    bool memory[32] = {0}, reserved[32] = {0}, timer[32] = {0}, chosen[32] = {0};
    bool wifi[32] = {0}, mac_seen[32] = {0};
    bool cpus = false, cpu = false, cpu_reg = false, cpu_enabled = true;
    uint64_t affinity = 0;

    struct
    {
        bool host, reg, irq;
        uint64_t base, size;
        uint32_t number;
    } sdhci[32] = {{0}};

    unsigned char mac[32][6] = {{0}};
    unsigned address_cells = 2, size_cells = 1;
    while (span(pos, 4, end))
    {
        uint32_t token = be32(b + pos);
        pos += 4;
        if (token == 1)
        { /* FDT_BEGIN_NODE */
            size_t n = length(b + pos, end - pos);
            if (n == end - pos || depth == 32 || root_closed)
                return false;
            if (!depth)
            {
                if (root_seen || n)
                    return false;
                root_seen = true;
            }
            if (depth == 1)
            {
                in_psci = equals(b + pos, n + 1, "psci");
                cpus = equals(b + pos, n + 1, "cpus");
            }
            if (depth == 2 && cpus)
            {
                cpu = n >= 4 && b[pos] == 'c' && b[pos + 1] == 'p' && b[pos + 2] == 'u' &&
                      b[pos + 3] == '@';
                cpu_reg = false;
                cpu_enabled = true;
            }
            memory[depth] = depth == 1 && n >= 7 && b[pos] == 'm' && b[pos + 1] == 'e' &&
                            b[pos + 2] == 'm' && b[pos + 3] == 'o' && b[pos + 4] == 'r' &&
                            b[pos + 5] == 'y' && (b[pos + 6] == '@' || b[pos + 6] == 0);
            reserved[depth] = depth == 1 ? equals(b + pos, n + 1, "reserved-memory")
                                         : (depth == 2 && reserved[1]);
            timer[depth] = depth == 1 && equals(b + pos, n + 1, "timer");
            chosen[depth] = depth == 1 && equals(b + pos, n + 1, "chosen");
            wifi[depth] = mac_seen[depth] = false;
            sdhci[depth].host = sdhci[depth].reg = sdhci[depth].irq = false;
            pos += (uint32_t)((n + 4) & ~(size_t)3);
            if (pos > end)
                return false;
            ++depth;
        }
        else if (token == 2)
        { /* FDT_END_NODE */
            if (!depth)
                return false;
            if (depth == 3 && cpus && cpu && cpu_reg && cpu_enabled)
            {
                if (out->cpu_count == 4)
                    return false;
                for (unsigned i = 0; i < out->cpu_count; ++i)
                    if (out->cpu_affinity[i] == affinity)
                        return false;
                out->cpu_affinity[out->cpu_count++] = affinity;
            }
            /* Root-level SDHCI resources use the root address space and the Pi5 GIC SPI
             * binding. Nested buses remain under the platform's firmware contract. */
            if (depth == 2 && sdhci[1].host && sdhci[1].reg &&
                sdhci[1].base == UINT64_C(0x1000fff000))
            {
                if (out->sdhci_valid || sdhci[1].size < 0x100 || !sdhci[1].irq)
                    return false;
                out->sdhci_base = sdhci[1].base;
                out->sdhci_size = sdhci[1].size;
                out->sdhci_irq = sdhci[1].number;
                out->sdhci_valid = true;
            }
            if (wifi[depth - 1] && mac_seen[depth - 1])
            {
                unsigned any = 0;
                for (unsigned i = 0; i < 6; ++i)
                    any |= mac[depth - 1][i];
                if (!any || (mac[depth - 1][0] & 1) || out->wifi_mac_valid)
                    return false;
                for (unsigned i = 0; i < 6; ++i)
                    out->wifi_mac[i] = mac[depth - 1][i];
                out->wifi_mac_valid = true;
            }
            if (depth == 2)
                in_psci = false;
            if (!--depth)
                root_closed = true;
        }
        else if (token == 3)
        { /* FDT_PROP */
            if (!depth || !span(pos, 8, end))
                return false;
            uint32_t n = be32(b + pos), name = be32(b + pos + 4);
            pos += 8;
            if (!span(pos, n, end) || name >= strings_size)
                return false;
            const unsigned char *key = b + strings + name;
            size_t key_len = length(key, strings_size - name);
            if (key_len == strings_size - name)
                return false;
            if (depth == 3 && cpus && cpu)
            {
                if (equals(key, key_len + 1, "reg"))
                {
                    if (n != 4 && n != 8)
                        return false;
                    affinity = n == 8 ? be64(b + pos) : be32(b + pos);
                    cpu_reg = true;
                }
                if (equals(key, key_len + 1, "status"))
                    cpu_enabled = contains(b + pos, n, "okay") || contains(b + pos, n, "ok");
            }
            if (depth == 2 && equals(key, key_len + 1, "reg") && address_cells == 2 &&
                size_cells == 2 && n == 16)
            {
                sdhci[1].base = be64(b + pos);
                sdhci[1].size = be64(b + pos + 8);
                if (sdhci[1].base > UINT64_MAX - sdhci[1].size)
                    return false;
                sdhci[1].reg = true;
            }
            if (depth == 2 && equals(key, key_len + 1, "interrupts") && n == 12 &&
                be32(b + pos) == 0 && be32(b + pos + 4) < 988 && be32(b + pos + 8) == 4)
            {
                sdhci[1].number = be32(b + pos + 4) + 32;
                sdhci[1].irq = true;
            }
            if (depth == 1 && equals(key, key_len + 1, "#address-cells"))
            {
                if (n != 4)
                    return false;
                address_cells = be32(b + pos);
            }
            if (depth == 1 && equals(key, key_len + 1, "#size-cells"))
            {
                if (n != 4)
                    return false;
                size_cells = be32(b + pos);
            }
            if ((memory[depth - 1] || (depth == 3 && reserved[depth - 1])) &&
                equals(key, key_len + 1, "reg"))
            {
                if (address_cells != 2 || size_cells != 2)
                    return false; /* Explicit Pi5 root/reserved-memory contract. */
                if (memory[depth - 1])
                {
                    if (!ranges(out->ram, &out->ram_count, 8, b + pos, n))
                        return false;
                }
                else if (!ranges(out->reserved, &out->reserved_count, 32, b + pos, n))
                    return false;
            }
            if (equals(key, key_len + 1, "compatible"))
            {
                sdhci[depth - 1].host = contains(b + pos, n, "qemu,sdhci") ||
                                        contains(b + pos, n, "brcm,bcm2712-sdhci");
                wifi[depth - 1] = contains(b + pos, n, "brcm,bcm4329-fmac");
                if (contains(b + pos, n, "brcm,bcm2712d0-pinctrl"))
                    out->bcm2712d0 = true;
                if (contains(b + pos, n, "arm,gic-400"))
                {
                    out->gic400 = true;
                }
                if (timer[depth - 1] && !contains(b + pos, n, "arm,armv8-timer"))
                    return false;
            }
            if (equals(key, key_len + 1, "local-mac-address"))
            {
                if (n == 6)
                {
                    for (unsigned i = 0; i < 6; ++i)
                        mac[depth - 1][i] = b[pos + i];
                    mac_seen[depth - 1] = true;
                }
            }
            if (timer[depth - 1] && equals(key, key_len + 1, "interrupts"))
            {
                for (uint32_t i = 0; i + 12 <= n; i += 12)
                    if (be32(b + pos + i) == 1 && be32(b + pos + i + 4) == 14)
                        out->physical_timer = true;
            }
            if (depth == 1 && equals(key, key_len + 1, "compatible"))
                out->pi5 = contains(b + pos, n, "raspberrypi,5-model-b");
            if (depth == 1 && equals(key, key_len + 1, "model"))
            {
                size_t len = length(b + pos, n);
                if (len == n)
                    return false;
                if (len >= sizeof(out->model))
                    len = sizeof(out->model) - 1;
                for (size_t i = 0; i < len; ++i)
                    out->model[i] = (char)b[pos + i];
                out->model[len] = 0;
            }
            if (depth == 2 && in_psci && equals(key, key_len + 1, "method"))
                out->psci_smc = equals(b + pos, n, "smc");
            if (chosen[depth - 1] && (equals(key, key_len + 1, "linux,initrd-start") ||
                                      equals(key, key_len + 1, "linux,initrd-end")))
            {
                if (n != 4 && n != 8)
                    return false;
                uint64_t value = n == 8 ? be64(b + pos) : be32(b + pos);
                if (equals(key, key_len + 1, "linux,initrd-start"))
                    out->initrd_start = value;
                else
                    out->initrd_end = value;
            }
            /* n is bounded by the <=2 MiB blob, so rounding cannot overflow. */
            pos += (n + 3) & ~3u;
            if (pos > end)
                return false;
        }
        else if (token == 9)
        { /* FDT_END */
            if (depth || !root_seen || !root_closed)
                return false;
            out->size = total;
            if ((out->initrd_start || out->initrd_end) &&
                (!out->initrd_start || out->initrd_start >= out->initrd_end ||
                 out->initrd_end > UINT64_C(0x200000000) ||
                 out->initrd_end - out->initrd_start > UINT64_C(0x4000000)))
                return false;
            return true;
        }
        else if (token != 4)
        { /* FDT_NOP */
            return false;
        }
    }
    return false;
}

bool lume_fdt_set_initrd(void *blob, size_t available, uint64_t start, uint64_t end)
{
    struct lume_fdt current;
    if (!start || end <= start || end > UINT64_C(0x200000000) || end - start > 0x4000000 ||
        !lume_fdt_read(blob, available, &current))
        return false;
    unsigned char *b = blob, *values[2] = {0};
    uint32_t sizes[2] = {0}, pos = be32(b + 8), strings = be32(b + 12);
    unsigned depth = 0;
    bool chosen = false;
    while (pos < be32(b + 8) + be32(b + 36))
    {
        uint32_t token = be32(b + pos);
        pos += 4;
        if (token == 1)
        {
            size_t n = length(b + pos, current.size - pos);
            if (depth == 1)
                chosen = equals(b + pos, n + 1, "chosen");
            ++depth;
            pos += (uint32_t)((n + 4) & ~(size_t)3);
        }
        else if (token == 2)
            --depth;
        else if (token == 3)
        {
            uint32_t n = be32(b + pos), name = be32(b + pos + 4);
            pos += 8;
            const unsigned char *key = b + strings + name;
            size_t key_size = length(key, current.size - strings - name) + 1;
            if (depth == 2 && chosen)
                for (unsigned i = 0; i < 2; ++i)
                    if (equals(key, key_size, i ? "linux,initrd-end" : "linux,initrd-start"))
                    {
                        if (values[i] || (n == 4 && (i ? end : start) > UINT32_MAX))
                            return false;
                        values[i] = b + pos;
                        sizes[i] = n;
                    }
            pos += (n + 3) & ~3u;
        }
        else if (token == 9)
            break;
    }
    if (!values[0] || !values[1])
        return false;
    for (unsigned i = 0; i < 2; ++i)
    {
        uint64_t value = i ? end : start;
        for (unsigned j = 0; j < sizes[i]; ++j)
            values[i][j] = (unsigned char)(value >> (8 * (sizes[i] - j - 1)));
    }
    return true;
}
