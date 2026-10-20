#ifndef LUME_CONSOLE_WAIT_H
#define LUME_CONSOLE_WAIT_H
#include <stdint.h>
#define CONSOLE_RENDER_PARCELS 2u
/* All producer/wait operations require the scheduler lock. IRQ consumers only
 * inspect the published mask; they never inspect a process or acquire it. */
extern volatile uint32_t console_wait_mask;
void console_wait_reset(unsigned);
int console_submit(unsigned, unsigned, const unsigned char *, unsigned);
void console_wait_collect(unsigned);
int console_wait_exiting(unsigned, uint32_t);
void console_wait_progress(uint32_t);
void console_wait_input_progress(uint32_t);
void console_wait_input_notify(uint32_t);
int console_render_pending(void);
int console_wait_work(unsigned, uint32_t);
int lume_io_work_ready(unsigned);
int lume_io_render_ready(void);
void lume_io_input_running(unsigned);
int console_wait_terminal(unsigned, unsigned, unsigned, uint32_t);
int console_wait_child(unsigned);
void console_wait_child_woken(unsigned);
/* Scheduler hooks; public process/context layout is unchanged. */
void lume_io_ready(unsigned);
void lume_io_work_resume(unsigned);
void lume_io_input_ready(unsigned);
int lume_io_render(unsigned, unsigned, const unsigned char *, unsigned);
void lume_io_exit_finish(unsigned, uint32_t);
unsigned console_kernel_take(unsigned char *, unsigned);
unsigned console_kernel_size(void);
#endif
