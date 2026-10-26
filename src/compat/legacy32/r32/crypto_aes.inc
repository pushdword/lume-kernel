/* The scheduler lock serializes admission, not packet processing. The device
 * lease survives a stack suspension and is bound to a slot/PID, like RNG work.
 * Authenticated output is copied only after all validation and driver success. */
static struct lume_crypto_device aes_device;
static unsigned aes_owner, aes_wait_mask;
static uint32_t aes_wait_pid[MAX_PROCESSES];

uint32_t lume_crypto_now(void)
{
    return uptime_ms;
}

void lume_crypto_checkpoint(void)
{
    lume_clock_checkpoint();
}

static int aes_acquire(unsigned slot)
{
    if (!aes_owner)
    {
        aes_owner = slot + 1u;
        return 1;
    }
    if (aes_owner == slot + 1u)
        panic("Recursive AES owner");
    aes_wait_mask |= 1u << slot;
    aes_wait_pid[slot] = processes[slot].pid;
    processes[slot].state = LUME_WAITING;
    return 0;
}

static void aes_release(unsigned slot)
{
    if (aes_owner != slot + 1u)
        panic("AES completion owner");
    aes_owner = 0;
    unsigned waiting = aes_wait_mask;
    aes_wait_mask = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (waiting & (1u << i))
        {
            if (processes[i].pid != aes_wait_pid[i] || processes[i].state != LUME_WAITING)
                continue;
            processes[i].context.regs[ARCH_REG_CALL] = LUME_CRYPTO;
            fs_pending[i] = (struct fs_pending){processes[i].pid, FS_RETRY_SYSCALL, 0, 0};
            processes[i].state = LUME_READY;
            lume_io_ready(i);
        }
}

static int aes_service(unsigned slot, unsigned op, const struct lume_crypto_request *request)
{
    if (op == LUME_CRYPTO_LIMITS)
    {
        if (request->length != sizeof(struct lume_crypto_limits))
            return -22;
        if (!user_range(slot, request->buffer, request->length, 1))
            return -14;
        struct lume_crypto_limits limits = {1,
                                            sizeof limits,
                                            LUME_AES_PACKET_MAX,
                                            LUME_AES_AAD_MAX,
                                            12,
                                            16,
                                            3,
                                            aes_device.available ? aes_device.clock_hz : 0};
        user_copy_to(slot, request->buffer, &limits, sizeof limits);
        return 0;
    }
    if (request->length != sizeof(struct lume_aead_request))
        return -22;
    if ((request->buffer & 3u) || !user_range(slot, request->buffer, request->length, 0))
        return -14;
    struct lume_aead_request r;
    user_copy_from(slot, &r, request->buffer, sizeof r);
    if (r.version != 1 || r.bytes != sizeof r || (r.key_bytes != 16 && r.key_bytes != 32) ||
        r.nonce_bytes != 12 || r.aad_bytes > LUME_AES_AAD_MAX || r.length > LUME_AES_PACKET_MAX)
        return -22;
    if (!user_range(slot, r.key, r.key_bytes, 0) || !user_range(slot, r.nonce, 12, 0) ||
        !user_range(slot, r.aad, r.aad_bytes, 0) || !user_range(slot, r.input, r.length, 0) ||
        !user_range(slot, r.output, r.length, 1) ||
        !user_range(slot, r.tag, 16, op == LUME_CRYPTO_AES_ENCRYPT))
        return -14;
    /* Range validation above makes subtraction/overlap checks non-wrapping. */
    if (r.length && r.output < r.tag + 16u && r.tag < r.output + r.length)
        return -22;
    if (!aes_device.available)
        return -95;
    if (!aes_acquire(slot))
        return 0;
    struct lume_aes_packet p = {.key_bytes = r.key_bytes,
                                .aad_bytes = r.aad_bytes,
                                .bytes = r.length,
                                .decrypt = op == LUME_CRYPTO_AES_DECRYPT,
                                .data = syscall_buffers[slot]};
    user_copy_from(slot, p.key, r.key, r.key_bytes);
    user_copy_from(slot, p.nonce, r.nonce, 12);
    user_copy_from(slot, p.aad, r.aad, r.aad_bytes);
    user_copy_from(slot, p.data, r.input, r.length);
    if (p.decrypt)
        user_copy_from(slot, p.tag, r.tag, 16);
    fs_release();
    unlock_kernel();
    int result = lume_crypto_device_packet(&aes_device, &p);
    lock_kernel();
    if (result == 0)
    {
        user_copy_to(slot, r.output, p.data, r.length);
        if (!p.decrypt)
            user_copy_to(slot, r.tag, p.tag, 16);
    }
    unlock_kernel();
    for (unsigned i = 0; i < r.length; i += 256)
    {
        unsigned n = r.length - i;
        if (n > 256)
            n = 256;
        lume_secret_clear(p.data + i, n);
        lume_clock_checkpoint();
    }
    lume_secret_clear(&p, sizeof p);
    lock_kernel();
    aes_release(slot);
    return result;
}
