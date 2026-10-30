#ifndef LUME_TTY_DEVICE_H
#define LUME_TTY_DEVICE_H
#include <stdbool.h>
#include "lume/tty.h"
struct lume_files;
int lume_tty_call(struct lume_files *, unsigned, unsigned, struct lume_tty_request *);
int64_t lume_tty_terminal(struct lume_files *, unsigned);
unsigned lume_tasks_tty_session(unsigned);
unsigned lume_tasks_tty_group(unsigned);
bool lume_tasks_tty_group_valid(unsigned, unsigned);
void lume_tasks_tty_signal(unsigned, unsigned, unsigned);
int lume_tasks_tty_background(unsigned, unsigned, unsigned, unsigned);
int lume_tty_access(struct lume_files *, unsigned, unsigned, bool);
#endif
