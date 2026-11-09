#include <stdbool.h>
void lume_worker_checkpoint(unsigned);
void lume_worker_compute(void (*)(void *), void *);
bool lume_worker_pause_ms(unsigned);

unsigned lume_worker_current(void);
