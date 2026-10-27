#include "../../arch/amd64/include/kernel/lume/smp.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf rejected;
static const char *failure;
static bool expected_failure;

_Noreturn void lume_fail(const char *message)
{
    assert(expected_failure);
    failure = message;
    longjmp(rejected, 1);
}

#include "../../arch/amd64/smp.c"

int main(void)
{
    assert(lume_cpu_count() == 1 && !lume_smp_owned() && !lume_smp_contended());
    assert(!lume_smp_lock_turn() && !lume_smp_wait_ticks() && !lume_smp_wait_total() &&
           !lume_smp_wait_start(0));
    for (uint64_t turn = 1; turn <= 32; ++turn)
    {
        lume_smp_lock();
        assert(lume_smp_owned() && !lume_smp_contended() && lume_smp_lock_turn() == turn);
        expected_failure = true;
        if (!setjmp(rejected))
        {
            lume_smp_lock();
            assert(false);
        }
        expected_failure = false;
        assert(!strcmp(failure, "recursive kernel lock"));
        assert(lume_smp_owned() && lume_smp_lock_turn() == turn);
        lume_smp_unlock();
        assert(!lume_smp_owned() && !lume_smp_contended() && lume_smp_lock_turn() == turn);
        expected_failure = true;
        if (!setjmp(rejected))
        {
            lume_smp_unlock();
            assert(false);
        }
        expected_failure = false;
        assert(!strcmp(failure, "kernel lock ownership"));
        assert(!lume_smp_owned() && lume_smp_lock_turn() == turn);
    }
    puts("SMP_LOCK_PASS recursion ownership turns=32 UP");
    return 0;
}
