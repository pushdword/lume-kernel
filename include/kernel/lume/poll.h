#ifndef LUME_POLL_H
#define LUME_POLL_H
#include <stdbool.h>
#include <stdint.h>
#include <fd.h>

struct lume_files;
#define LUME_POLL_TASK_LIMIT 32u

/* Source mutation is serialized by the caller. Sources remain allocated until all
 * subscriptions to their previous generation have been cancelled. */
struct lume_poll_source
{
    uint64_t generation;
    uint32_t waiting;
};

void lume_poll_source_reset(struct lume_poll_source *);
void lume_poll_source_notify(struct lume_poll_source *);

struct lume_poll_watch
{
    struct lume_pollfd records[LUME_FD_LIMIT];
    uint64_t generations[LUME_FD_LIMIT];
    unsigned count;
};

void lume_vfs_poll_prepare(struct lume_files *, struct lume_poll_watch *);
unsigned lume_vfs_poll_check(struct lume_files *, struct lume_poll_watch *);
bool lume_vfs_poll_subscribe(struct lume_files *, const struct lume_poll_watch *, unsigned);
bool lume_vfs_poll_take(unsigned);
void lume_vfs_poll_cancel(unsigned);
void lume_vfs_poll_input(bool ready);
bool lume_vfs_poll_input_ready(void);
bool lume_vfs_poll_console(const struct lume_files *, const struct lume_poll_watch *);
#endif
