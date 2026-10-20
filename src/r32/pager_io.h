#ifndef LUME_PAGER_IO_H
#define LUME_PAGER_IO_H
/* Only after a complete metadata mutation. Boot/legacy host builds stay local. */
#if defined(LUME_KERNEL_PERF) || defined(LUME_PAGER_CHECKPOINT_TEST)
void lume_pager_checkpoint(void);
static inline void pager_safe_point(void){lume_pager_checkpoint();}
static inline void pager_checkpoint(unsigned iteration){if((iteration&63u)==63u)pager_safe_point();}
#else
static inline void pager_safe_point(void){}
static inline void pager_checkpoint(unsigned iteration){(void)iteration;}
#endif
#endif
