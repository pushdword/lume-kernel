#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LUME_TASK_LIMIT 16u
#include "io_queue.h"

static void order_and_cancel(void)
{
    struct io_queue queue = {0};
    for (unsigned n = 0; n < 1000; ++n)
    {
        for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        {
            assert(io_queue_push(&queue, task, task & 1));
            assert(!io_queue_push(&queue, task, false));
        }
        assert(!io_queue_push(&queue, LUME_TASK_LIMIT, false));
        io_queue_cancel(&queue, 7);
        io_queue_cancel(&queue, 0);
        io_queue_cancel(&queue, 15);
        assert(io_queue_push(&queue, 7, true));
        struct io_ticket ticket = {0};
        for (unsigned task = 1; task < 15; ++task)
            if (task != 7)
            {
                assert(io_queue_pop(&queue, &ticket));
                assert(ticket.task == task && ticket.replay == (bool)(task & 1));
            }
        assert(io_queue_pop(&queue, &ticket) && ticket.task == 7 && ticket.replay);
        assert(!io_queue_pop(&queue, &ticket) && !queue.count);
    }
}

static void grants(void)
{
    struct io_queue queue = {0};
    struct io_ticket ticket = {0};
    assert(io_queue_push(&queue, 2, true));
    assert(io_queue_push(&queue, 3, false));
    assert(io_queue_pop(&queue, &ticket) && ticket.task == 2 && ticket.replay);
    io_queue_grant(&queue, ticket);
    assert(io_queue_granted(&queue, 1000000));
    assert(!io_queue_push(&queue, 2, false));
    assert(!io_queue_claim(&queue, 3, 1000000));
    io_queue_arm(&queue, 2, 1000000, 10);
    assert(io_queue_granted(&queue, 1000009));
    assert(!io_queue_granted(&queue, 1000010));
    assert(io_queue_pop(&queue, &ticket) && ticket.task == 3);

    /* A handler may consume admission before the interrupted syscall returns. */
    assert(io_queue_push(&queue, 2, true));
    assert(io_queue_pop(&queue, &ticket));
    io_queue_grant(&queue, ticket);
    io_queue_arm(&queue, 2, 200, 10);
    assert(io_queue_claim(&queue, 2, 201));
    assert(!io_queue_claim(&queue, 2, 202));
    assert(io_queue_push(&queue, 3, false));
    assert(io_queue_push(&queue, 2, true));
    assert(io_queue_pop(&queue, &ticket) && ticket.task == 3);
    assert(io_queue_pop(&queue, &ticket) && ticket.task == 2);
    io_queue_grant(&queue, ticket);
    uint64_t old = ticket.generation;
    io_queue_cancel(&queue, 2);
    assert(!io_queue_claim(&queue, 2, 203));
    assert(io_queue_push(&queue, 2, true));
    assert(io_queue_pop(&queue, &ticket) && ticket.generation != old);
    io_queue_grant(&queue, ticket);
    ++queue.generation[2];
    assert(!io_queue_granted(&queue, 0));

    /* Replenishing direct I/O cannot overtake an earlier special syscall. */
    for (unsigned n = 0; n < 1000; ++n)
    {
        queue = (struct io_queue){0};
        assert(io_queue_push(&queue, 0, false));
        assert(io_queue_push(&queue, 1, true));
        assert(io_queue_push(&queue, 2, false));
        assert(io_queue_pop(&queue, &ticket) && ticket.task == 0);
        assert(io_queue_push(&queue, 0, false));
        assert(io_queue_pop(&queue, &ticket) && ticket.task == 1);
        io_queue_grant(&queue, ticket);
        io_queue_arm(&queue, 1, n * 20, 10);
        assert(io_queue_claim(&queue, 1, n * 20 + 1));
        assert(io_queue_pop(&queue, &ticket) && ticket.task == 2);
        assert(io_queue_pop(&queue, &ticket) && ticket.task == 0);
    }
}

static void model(void)
{
    struct io_queue queue = {0};
    unsigned expected[LUME_TASK_LIMIT], used = 0, random = 17;
    bool present[LUME_TASK_LIMIT] = {0};
    for (unsigned n = 0; n < 200000; ++n)
    {
        random = random * 1664525u + 1013904223u;
        unsigned task = (random >> 16) % LUME_TASK_LIMIT;
        if ((random & 3) == 0)
        {
            bool accepted = io_queue_push(&queue, task, false);
            assert(accepted == !present[task]);
            if (accepted)
            {
                expected[used++] = task;
                present[task] = true;
            }
        }
        else if ((random & 3) == 1)
        {
            io_queue_cancel(&queue, task);
            for (unsigned i = 0; i < used; ++i)
                if (expected[i] == task)
                {
                    memmove(expected + i, expected + i + 1, (--used - i) * sizeof(*expected));
                    break;
                }
            present[task] = false;
        }
        else
        {
            struct io_ticket ticket = {0};
            assert(io_queue_pop(&queue, &ticket) == (used != 0));
            if (used)
            {
                assert(ticket.task == expected[0]);
                present[ticket.task] = false;
                memmove(expected, expected + 1, --used * sizeof(*expected));
            }
        }
        assert(queue.count == used);
    }
}

int main(void)
{
    order_and_cancel();
    grants();
    model();
    assert(io_queue_direct(LUME_SYS_READ) && io_queue_direct(LUME_SYS_FILE_MODE));
    assert(!io_queue_direct(LUME_SYS_FORK) && !io_queue_direct(LUME_SYS_THREAD) &&
           !io_queue_direct(LUME_SYS_SIGNAL) && !io_queue_direct(LUME_SYS_FD));
    puts("PASS_IO_QUEUE FIFO, generations, cancellation, bounded admission and progress");
    return 0;
}
