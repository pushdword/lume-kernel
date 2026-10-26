#include "supervision_bus.h"
#include <assert.h>
#include <stdio.h>

static struct lume_supervision_request request(unsigned command, const char *name)
{
    struct lume_supervision_request r = {.version = LUME_SUPERVISION_VERSION, .command = command};
    if (name)
        strcpy(r.name, name);
    return r;
}

int main(void)
{
    struct supervision_bus bus = {0};
    struct lume_supervision_request r = request(LUME_SV_LIST, NULL);
    assert(supervision_call(&bus, 3, LUME_SV_SUBMIT, false, true, &r) == -LUME_ENOENT);
    assert(supervision_call(&bus, 2, LUME_SV_REGISTER, false, true, &r) == -LUME_EPERM);
    assert(supervision_call(&bus, 2, LUME_SV_REGISTER, true, false, &r) == 0);
    assert(supervision_call(&bus, 4, LUME_SV_REGISTER, true, false, &r) == -LUME_EBUSY);
    assert(supervision_call(&bus, 3, LUME_SV_SUBMIT, false, false, &r) == -LUME_EPERM);
    r = request(LUME_SV_START, "../bad");
    assert(supervision_call(&bus, 3, LUME_SV_SUBMIT, false, true, &r) == -LUME_EINVAL);
    r = request(LUME_SV_START, "sshd");
    assert(supervision_call(&bus, 3, LUME_SV_SUBMIT, false, true, &r) == 0);
    uint64_t first = r.sequence;
    struct lume_supervision_request duplicate = request(LUME_SV_LIST, NULL);
    assert(supervision_call(&bus, 3, LUME_SV_SUBMIT, false, true, &duplicate) == -LUME_EBUSY);
    assert(supervision_call(&bus, 4, LUME_SV_RESULT, false, true, &r) == -LUME_ENOENT);
    assert(supervision_call(&bus, 3, LUME_SV_RESULT, false, true, &r) == -LUME_EAGAIN);
    struct lume_supervision_request second = request(LUME_SV_LIST, NULL);
    assert(supervision_call(&bus, 4, LUME_SV_SUBMIT, false, true, &second) == 0);
    assert(second.sequence > first);
    assert(supervision_call(&bus, 3, LUME_SV_RECEIVE, false, true, &r) == -LUME_EPERM);
    assert(supervision_call(&bus, 2, LUME_SV_RECEIVE, true, false, &r) == 0);
    assert(r.sequence == first && r.command == LUME_SV_START && !strcmp(r.name, "sshd"));
    r.count = LUME_SUPERVISION_COUNT + 1;
    assert(supervision_call(&bus, 2, LUME_SV_REPLY, true, false, &r) == -LUME_EINVAL);
    r.count = 1;
    strcpy(r.service[0].name, "sshd");
    r.service[0].state = LUME_SV_RUNNING;
    r.service[0].pid = 7;
    assert(supervision_call(&bus, 2, LUME_SV_REPLY, true, false, &r) == 0);
    assert(supervision_call(&bus, 3, LUME_SV_RESULT, false, true, &r) == 0);
    assert(r.count == 1 && r.service[0].pid == 7);
    assert(supervision_call(&bus, 3, LUME_SV_RESULT, false, true, &r) == -LUME_ENOENT);
    assert(supervision_call(&bus, 2, LUME_SV_RECEIVE, true, false, &r) == 0);
    assert(r.sequence == second.sequence);
    supervision_release(&bus, 4);
    assert(supervision_call(&bus, 2, LUME_SV_REPLY, true, false, &r) == -LUME_ENOENT);
    assert(supervision_call(&bus, 4, LUME_SV_RESULT, false, true, &r) == -LUME_ENOENT);
    second = request(LUME_SV_STATUS, "sshd");
    assert(supervision_call(&bus, 4, LUME_SV_SUBMIT, false, true, &second) == 0);
    assert(second.sequence > r.sequence);
    supervision_release(&bus, 2);
    assert(!bus.manager);
    assert(supervision_call(&bus, 4, LUME_SV_RESULT, false, true, &second) == 0);
    assert(second.result == -LUME_EPIPE && !second.count);
    assert(supervision_call(&bus, 5, LUME_SV_REGISTER, true, false, &r) == 0);
    r = request(LUME_SV_STATUS, "sshd");
    assert(supervision_call(&bus, 4, LUME_SV_SUBMIT, false, true, &r) == 0);
    assert(supervision_call(&bus, 4, LUME_SV_CANCEL, false, true, &r) == 0);
    assert(supervision_call(&bus, 5, LUME_SV_RECEIVE, true, false, &r) == -LUME_EAGAIN);
    for (unsigned i = 0; i < LUME_SUPERVISION_COUNT; ++i)
    {
        r = request(LUME_SV_LIST, NULL);
        assert(supervision_call(&bus, 10 + i, LUME_SV_SUBMIT, false, true, &r) == 0);
    }
    r = request(LUME_SV_LIST, NULL);
    assert(supervision_call(&bus, 30, LUME_SV_SUBMIT, false, true, &r) == -LUME_EAGAIN);
    supervision_release(&bus, 10);
    assert(supervision_call(&bus, 30, LUME_SV_SUBMIT, false, true, &r) == 0);
    puts("PASS supervision authorization, ordering, cancellation and process reuse");
    return 0;
}
