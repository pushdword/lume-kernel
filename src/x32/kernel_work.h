#ifndef LUME_KERNEL_WORK_H
#define LUME_KERNEL_WORK_H
/* Private resumable kernel work; public176B context/368B process stay intact.
 * Suspended stacks remain bound to their original core. No C frame migrates. */
#define KWORK_STACK_WORDS 4096u
#define KWORK_CANARY 0x4b53544bu
enum {KW_NONE,KW_RUNNING,KW_SUSPENDED,KW_DONE};
enum {KW_SYSCALL=1,KW_FAULT,KW_RETURN,KW_EXIT,KW_FINISH_EXIT};
struct kernel_work {
    uint32_t sp,pid,core,state,kind,arg,result;
    uint32_t epoch,slow,started,rejected,waiting,wait_ticks,pager_after;
};
static struct kernel_work kernel_work[MAX_PROCESSES];
static uint32_t kernel_stacks[MAX_PROCESSES][KWORK_STACK_WORDS] __attribute__((aligned(16)));
static uint32_t scheduler_sp[2],kernel_work_mask;
static int active_work[2]={-1,-1};
static uint32_t work_pauses[2],work_resumes[2];
extern void x32_kernel_switch(uint32_t *,uint32_t *);
static int work_call(unsigned,unsigned,uint32_t);
static void work_preempt(void);
static int work_suspended(unsigned slot){return slot<MAX_PROCESSES&&kernel_work[slot].state==KW_SUSPENDED;}
static int work_eligible(unsigned slot,unsigned core){return !work_suspended(slot)||kernel_work[slot].core==core;}
static inline __attribute__((always_inline)) void release_user_pins(void){
    if(!kernel_work_mask){user_unpin();return;}
    int slot=current[core_id()];
    if(slot<0||!(kernel_work_mask&(1u<<(unsigned)slot))||!work_suspended((unsigned)slot))user_unpin();
}
#endif
