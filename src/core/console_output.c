#include "lume/console_output.h"
#include "lume/ticket_lock.h"
#include "lume/cpu.h"
#include <stdbool.h>

static struct lume_ticket_lock serial;
static bool initialized;

void lume_console_output_start(void)
{
    initialized = true;
}

uint64_t lume_console_serial_reserve(void)
{
    if (!initialized)
        return 0;
    return UINT64_C(0x100000000) | lume_ticket_reserve(&serial);
}

void lume_console_serial_wait(uint64_t ticket)
{
    if (ticket)
        while (!lume_ticket_ready(&serial, (uint32_t)ticket))
            lume_cpu_relax_ordered();
}

uint64_t lume_console_serial_lock(void)
{
    uint64_t ticket = lume_console_serial_reserve();
    lume_console_serial_wait(ticket);
    return ticket;
}

void lume_console_serial_unlock(uint64_t ticket)
{
    if (ticket)
    {
        lume_memory_complete();
        lume_ticket_release(&serial, (uint32_t)ticket);
    }
}
