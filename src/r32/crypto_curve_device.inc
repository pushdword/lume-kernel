/* Optional complete Curve25519 engine. Private kernel buffers only; SYS-domain
 * device lease is separate from AES/RNG. Never issue user-space MMIO or SPI. */
int lume_curve_device_discover(struct lume_crypto_device *out, uint32_t timer_hz)
{
    *out = (struct lume_crypto_device){0};
    uintptr_t table = 0xf0002000u;
    uint32_t h[8];
    for (unsigned i = 0; i < 8; i++)
        h[i] = lume_crypto_mmio_read(table + 4u * i);
    if (h[0] != 0x54445748u || h[1] != 1 || h[2] != 32 || h[3] != 32 || !h[4] || h[4] > 126 ||
        h[5] != 32u + 32u * h[4] || h[6] != timer_hz || h[7])
        return -19;
    unsigned found = 0;
    for (unsigned i = 0; i < h[4]; i++)
    {
        uintptr_t p = table + 32u + 32u * i;
        if (lume_crypto_mmio_read(p) != 13 || lume_crypto_mmio_read(p + 4))
            continue;
        if (found++)
            return -22;
        if (lume_crypto_mmio_read(p + 8) != 1 || lume_crypto_mmio_read(p + 12) != 0xf0003100u ||
            lume_crypto_mmio_read(p + 16) != 192 || lume_crypto_mmio_read(p + 20) != UINT32_MAX ||
            lume_crypto_mmio_read(p + 24) != timer_hz || lume_crypto_mmio_read(p + 28) != 7)
            return -95;
    }
    if (!found)
        return -19;
    uintptr_t base = 0xf0003100u;
    if (lume_crypto_mmio_read(base) != 0x31565243u || lume_crypto_mmio_read(base + 4) != 1 ||
        lume_crypto_mmio_read(base + 8) != 7)
        return -95;
    *out = (struct lume_crypto_device){(uint32_t)base, timer_hz, 1};
    return 0;
}

static int curve_wait(struct lume_crypto_device *d, unsigned completion, uint32_t *result)
{
    lume_crypto_checkpoint();
    uint32_t started = lume_crypto_now();
    unsigned polls = 0;
    for (;;)
    {
        uint32_t status = lume_crypto_mmio_read(d->base + 12u);
        if (status & ~15u)
            return -5;
        if (!(status & 1u) && (!completion || (status & 2u)))
        {
            *result = status;
            return 0;
        }
        if (!(++polls & 31u))
        {
            lume_crypto_checkpoint();
            if ((uint32_t)(lume_crypto_now() - started) > 5000u)
                return -110;
        }
    }
}

static int curve_clear(struct lume_crypto_device *d)
{
    lume_crypto_mmio_write(d->base + 16u, 4);
    uint32_t status;
    int r = curve_wait(d, 0, &status);
    return r < 0 ? r : status ? -5 : 0;
}

int lume_curve_device_packet(struct lume_crypto_device *d, struct lume_curve_packet *p)
{
    if (!d->available)
        return -95;
    if (p->operation < 1 || p->operation > 3)
        return -22;
    int result = curve_clear(d);
    unsigned char output[32] = {0};
    uint32_t cycles = 0;
    if (result >= 0)
    {
        for (unsigned i = 0; i < 32; i += 4)
        {
            lume_crypto_mmio_write(d->base + 64u + i, load_word(p->scalar_a + i, 4));
            if (p->operation == 3)
                lume_crypto_mmio_write(d->base + 96u + i, load_word(p->scalar_b + i, 4));
            if (p->operation != 2)
                lume_crypto_mmio_write(d->base + 128u + i, load_word(p->point + i, 4));
        }
        lume_secret_clear(p->scalar_a, 32);
        lume_secret_clear(p->scalar_b, 32);
        lume_crypto_mmio_write(d->base + 16u, p->operation);
        uint32_t status;
        result = curve_wait(d, 1, &status);
        if (result >= 0)
        {
            if (status == 10u)
                result = -74;
            else if (status != 6u)
                result = -5;
            else
            {
                for (unsigned i = 0; i < 32; i += 4)
                    store_word(output + i, lume_crypto_mmio_read(d->base + 160u + i), 4);
                cycles = lume_crypto_mmio_read(d->base + 24u);
                result = 0;
            }
        }
    }
    int wiped = curve_clear(d);
    if (wiped < 0)
    {
        d->available = 0;
        result = wiped;
    }
    if (result == 0)
    {
        memcpy(p->output, output, 32);
        p->cycles = cycles;
    }
    lume_secret_clear(output, 32);
    return result;
}

static void curve_test_decode(unsigned char out[32], const char *hex)
{
    for (unsigned i = 0; i < 32; i++)
    {
        unsigned a = (unsigned)(unsigned char)hex[2 * i],
                 b = (unsigned)(unsigned char)hex[2 * i + 1];
        out[i] = (unsigned char)(((a <= '9' ? a - '0' : a - 'a' + 10u) << 4) |
                                 (b <= '9' ? b - '0' : b - 'a' + 10u));
    }
}

int lume_curve_device_selftest(struct lume_crypto_device *d)
{
    static const unsigned char n[32] = {0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
                                        0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
                                        0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
                                        0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
    static const unsigned char u[32] = {0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
                                        0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
                                        0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
                                        0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
    static const unsigned char expected[32] = {0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
                                               0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
                                               0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
                                               0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};
    struct lume_curve_packet p = {.operation = 1};
    memcpy(p.scalar_a, n, 32);
    memcpy(p.point, u, 32);
    int r = lume_curve_device_packet(d, &p);
    if (r == 0 && memcmp(p.output, expected, 32))
        r = -5;
    unsigned char check[32];
    if (r == 0)
    {
        p.operation = 2;
        curve_test_decode(p.scalar_a,
                          "307c83864f2833cb427a2ef1c00a013cfdff2768d980c0a3a520f006904de94f");
        curve_test_decode(check,
                          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        r = lume_curve_device_packet(d, &p);
        if (r == 0 && memcmp(p.output, check, 32))
            r = -5;
    }
    if (r == 0)
    {
        p.operation = 3;
        curve_test_decode(p.scalar_a,
                          "86eabc8e4c96193d290504e7c600df6cf8d8256131ec2c138a3e7e162e525404");
        curve_test_decode(p.scalar_b,
                          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        curve_test_decode(p.point,
                          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        curve_test_decode(check,
                          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155");
        r = lume_curve_device_packet(d, &p);
        if (r == 0 && memcmp(p.output, check, 32))
            r = -5;
    }
    lume_secret_clear(check, 32);
    lume_secret_clear(&p, sizeof p);
    if (r < 0)
        d->available = 0;
    return r;
}
