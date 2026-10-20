#ifndef LUME_TERMINAL_H
#define LUME_TERMINAL_H
#include <stdint.h>
#define TERM_COLUMNS 128u
#define TERM_ROWS 48u
#define TERM_PLAIN_BATCH 1024u
#define TERM_CONTROL_BATCH 128u
struct terminal {unsigned row,column,saved_row,saved_column,state,count,private_mode,wrap;
    unsigned parameters[8],foreground,background,inverse,columns,rows,utf8,utf8_left,utf8_min;volatile uint32_t *cells;
    unsigned cursor_hidden,attribute,erase_first,erase_end,erase_blank,batch;
    unsigned watch_at,watch_cell,watch_active,watch_touched;};
#ifdef LUME_VIDEO_IO_TEST
uint32_t terminal_mmio_read(volatile uint32_t *);
void terminal_mmio_write(volatile uint32_t *,uint32_t);
#else
static inline uint32_t terminal_mmio_read(volatile uint32_t *p){return *p;}
static inline void terminal_mmio_write(volatile uint32_t *p,uint32_t value){*p=value;}
#endif
void terminal_start(struct terminal *,volatile uint32_t *,unsigned,unsigned);
void terminal_byte(struct terminal *,unsigned);
/* A batch defers only one blank interval, never a screen-sized RAM image. */
void terminal_begin(struct terminal *);
void terminal_end(struct terminal *);
void terminal_write(struct terminal *,const unsigned char *,unsigned);
unsigned terminal_cell(const struct terminal *,unsigned);
struct keymap {unsigned caps,dead;};
/* One press event, USB boot usage + modifier byte. Returns UTF-8/ANSI bytes. */
unsigned keymap_pt(struct keymap *,unsigned,unsigned,unsigned char [16]);
/* Two independent screen panes; tty0 also owns the existing UART stream. */
void console_render_begin(void);
void console_render_end(void);
void console_write_tty(unsigned,const unsigned char *,unsigned);
int console_getchar_tty(unsigned);
int console_ready_tty(unsigned);
unsigned console_geometry_tty(unsigned);
unsigned console_tty_supported(unsigned);
#endif
