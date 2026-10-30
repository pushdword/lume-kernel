#ifndef LUME_POLL_STATE_H
#define LUME_POLL_STATE_H
#include "lume/poll.h"

struct poll_wait_state
{
    struct lume_poll_watch watch;
    uint64_t request, output, deadline, signal_epoch;
    unsigned operation;
    bool active, check_epoch, tty_read;
};
#endif
