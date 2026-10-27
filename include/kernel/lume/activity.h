#ifndef LUME_KERNEL_ACTIVITY_H
#define LUME_KERNEL_ACTIVITY_H
#include "../../uapi/lume/activity.h"

unsigned lume_activity_enter(unsigned kind);
unsigned lume_activity_switch(unsigned kind);
void lume_activity_leave(unsigned previous);
int lume_activity_snapshot(unsigned operation, struct lume_activity_snapshot *out);
#endif
