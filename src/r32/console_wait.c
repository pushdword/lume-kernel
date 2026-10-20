#include "core.h"
#include "terminal.h"
#include "uart_queue.h"
#include "console_wait.h"
#include <string.h>
enum {NONE,OUTPUT,INPUT,DRAIN,DIAGNOSTIC,EXITING};
struct waiter {
    uint32_t pid,generation,reason,resume,deadline,code;
    uint32_t output_sequence,log_sequence;
    unsigned tty,output_size,log_size;
    unsigned output_at,log_at,output_admitted,log_admitted,work_started;
    unsigned char output[4096],log[4096];
};
static struct waiter waiters[MAX_PROCESSES];
static uint32_t sequence,buffered_mask,input_mask,input_poll_after;
static unsigned cached_head[2],cached_logs[2],heads_dirty=3;
#ifdef LUME_CONSOLE_WAIT_TEST
static unsigned head_rebuilds[2];
#endif
static unsigned render_busy,render_turn;
volatile uint32_t console_wait_mask;
static uint32_t stamp(void){if(!++sequence)++sequence;return sequence;}
static int reached(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static unsigned render_span(const unsigned char *p,unsigned n){
    unsigned wide=n>TERM_PLAIN_BATCH?TERM_PLAIN_BATCH:n,i=0;
    for(;i<wide;i++)if(p[i]!='\n'&&(p[i]<32||p[i]>=127))break;
    if(i>TERM_CONTROL_BATCH)return i;
    if(n>TERM_CONTROL_BATCH)n=TERM_CONTROL_BATCH;
    for(i=0;i<n;i++)if(p[i]=='\n')return i+1;return n;
}
void console_wait_reset(unsigned slot){
    if(render_busy==slot+1u)panic("Reset live renderer");
    uint32_t generation=waiters[slot].generation+1;
    /* Payload is private and read only up to the freshly published sizes.
     * Reusing a slot needs64B of metadata reset, not an8KiB locked clear. */
    memset(&waiters[slot],0,offsetof(struct waiter,output));waiters[slot].generation=generation;
    waiters[slot].pid=processes[slot].pid;heads_dirty=3;input_mask&=~(1u<<slot);console_wait_mask&=~(1u<<slot);buffered_mask&=~(1u<<slot);
}
static void hold(unsigned slot,unsigned reason){
    struct waiter *w=waiters+slot;struct process *p=processes+slot;
    if(!(console_wait_mask&(1u<<slot)))w->resume=p->state==LUME_RUNNING?LUME_READY:p->state;
    w->reason=reason;input_mask&=~(1u<<slot);if(reason==INPUT){input_mask|=1u<<slot;input_poll_after=uptime_ms;}p->state=LUME_WAITING;
    __asm__ volatile("":::"memory");console_wait_mask|=1u<<slot;
}
static void resume(unsigned slot,unsigned interactive){
    struct waiter *w=waiters+slot;struct process *p=processes+slot;
    unsigned running=p->state==LUME_RUNNING,reason=w->reason;
    w->reason=NONE;input_mask&=~(1u<<slot);console_wait_mask&=~(1u<<slot);
    if(reason==EXITING){lume_io_exit_finish(slot,w->code);return;}
    p->state=running&&w->resume==LUME_READY?LUME_RUNNING:w->resume;
    if(p->state==LUME_READY){if(interactive)lume_io_input_ready(slot);else lume_io_ready(slot);}
    else if(p->state==LUME_RUNNING&&interactive)lume_io_input_running(slot);
}
int console_wait_child(unsigned slot){
    struct waiter *w=waiters+slot;
    if((console_wait_mask&(1u<<slot))&&w->reason==DIAGNOSTIC)return w->resume==LUME_WAITING;
    return processes[slot].state==LUME_WAITING&&!(console_wait_mask&(1u<<slot));
}
void console_wait_child_woken(unsigned slot){
    if(console_wait_mask&(1u<<slot))waiters[slot].resume=LUME_READY;
    else{processes[slot].state=LUME_READY;lume_io_ready(slot);}
}
/* Sequence ordering is per pane; a logical write remains its head until all
 * parcels complete. At most16 live sequence numbers make wrap comparison safe. */
static unsigned head(unsigned tty,unsigned *logs){
    unsigned bit=1u<<tty,slot=cached_head[tty],kind=cached_logs[tty];
    if(!(heads_dirty&bit)&&(slot==MAX_PROCESSES||
       (waiters[slot].pid==processes[slot].pid&&(kind?waiters[slot].log_size:waiters[slot].output_size)))){
        *logs=kind;return slot;
    }
    unsigned selected=MAX_PROCESSES;uint32_t oldest=0;
    for(unsigned pending=buffered_mask,at=0;pending&&at<MAX_PROCESSES;at++,pending>>=1){
        if(!(pending&1u))continue;
        struct waiter *w=waiters+at;
        if(w->pid!=processes[at].pid){console_wait_reset(at);continue;}
        for(unsigned k=0;k<2;k++){
            unsigned size=k?w->log_size:w->output_size;uint32_t seq=k?w->log_sequence:w->output_sequence;
            if(size&&(k?0:w->tty)==tty&&(selected==MAX_PROCESSES||(int32_t)(seq-oldest)<0)){
                selected=at;oldest=seq;kind=k;
            }
        }
    }
    cached_head[tty]=selected;cached_logs[tty]=kind;heads_dirty&=~bit;*logs=kind;
#ifdef LUME_CONSOLE_WAIT_TEST
    head_rebuilds[tty]++;
#endif
    return selected;
}
static unsigned selected_work(unsigned *logs,unsigned *tty){
    for(unsigned pass=0;pass<2;pass++){
        unsigned pane=render_turn^pass,kind=0,slot=head(pane,&kind);
        if(slot==MAX_PROCESSES)continue;
        struct waiter *w=waiters+slot;
        if(kind?w->log_admitted:w->output_admitted){*logs=kind;*tty=pane;return slot;}
    }return MAX_PROCESSES;
}
int console_render_pending(void){return (int)buffered_mask;}
static void finish_wait(unsigned slot,uint32_t now){
    if(!(console_wait_mask&(1u<<slot))||render_busy==slot+1u)return;
    struct waiter *w=waiters+slot;
    if(w->pid!=processes[slot].pid){console_wait_reset(slot);return;}
    if(w->reason==INPUT){
        if(w->log_size)return; /* Preserve diagnostic completion before user return. */
        if(console_ready_tty(w->tty)){processes[slot].context.regs[ARCH_REG_RESULT]=0;resume(slot,1);}
        else if(reached(now,w->deadline)){processes[slot].context.regs[ARCH_REG_RESULT]=(uint32_t)-11;resume(slot,0);}
    }else if(w->reason==DRAIN){
        int result=uart_queue_drain();
        if(result!=-11||reached(now,w->deadline)){processes[slot].context.regs[ARCH_REG_RESULT]=(uint32_t)(result==-11?-110:result);resume(slot,0);}
    }else{
        if(w->output_size&&!w->output_admitted&&reached(now,w->deadline)){
            w->output_size=w->output_at=0;heads_dirty|=1u<<w->tty;processes[slot].context.regs[ARCH_REG_RESULT]=(uint32_t)-110;
        }
        if(!w->log_size&&!w->output_size){buffered_mask&=~(1u<<slot);resume(slot,0);}
    }
}
/* IPIs include UART arrival: a fresh event must bypass the quiet poll delay.
 * Caller holds queue_lock, unlike the raw UART IRQ service path. */
void console_wait_input_notify(uint32_t now){input_poll_after=now;}
void console_wait_input_progress(uint32_t now){
    if(!input_mask||!reached(now,input_poll_after))return;
    input_poll_after=now+5u;
    for(unsigned pending=input_mask,slot=0;pending&&slot<MAX_PROCESSES;slot++,pending>>=1)
        if(pending&1u)finish_wait(slot,now);
}
void console_wait_progress(uint32_t now){
    if(!(console_wait_mask|buffered_mask))return;
    console_wait_input_progress(now);
    /* Admission never renders or drops queue_lock. The selected producer is
     * made runnable for an explicit continuation, not resumed in userspace. */
    for(unsigned tty=0;buffered_mask&&tty<2;tty++){
        unsigned logs=0,slot=head(tty,&logs);if(slot==MAX_PROCESSES)continue;
        struct waiter *w=waiters+slot;
        unsigned *admitted=logs?&w->log_admitted:&w->output_admitted;
        unsigned n=logs?w->log_size:w->output_size;
        if(*admitted)continue;
        int result=!tty&&uart_queue_active()?uart_queue_submit(logs?w->log:w->output,n):(int)n;
        if(result>=0)*admitted=1;
        else if(result!=-11){
            if(logs)panic("UART diagnostic transport stalled");
            w->output_size=w->output_at=0;heads_dirty|=1u<<tty;processes[slot].context.regs[ARCH_REG_RESULT]=(uint32_t)result;
            if(!w->log_size)buffered_mask&=~(1u<<slot);
        }
    }
    for(unsigned pending=console_wait_mask&~input_mask,slot=0;pending&&slot<MAX_PROCESSES;slot++,pending>>=1)if(pending&1u)finish_wait(slot,now);
    if(!buffered_mask||render_busy||!lume_io_render_ready())return;
    unsigned logs=0,tty=0,slot=selected_work(&logs,&tty);(void)logs;(void)tty;
    if(slot==MAX_PROCESSES||!lume_io_work_ready(slot))return;
    struct waiter *w=waiters+slot;struct process *p=processes+slot;
    if(p->state==LUME_SLEEPING||(p->state==LUME_WAITING&&!(console_wait_mask&(1u<<slot))))hold(slot,DIAGNOSTIC);
    if(p->state==LUME_WAITING){
        p->state=LUME_READY;
        /* A continuation stays runnable: no new sleep credit or wake scan for
         * each parcel of the same logical output operation. */
        if(w->work_started)lume_io_work_resume(slot);else{w->work_started=1;lume_io_ready(slot);}
    }
}
/* Called only for a selected RUNNING producer, before IRET. No live syscall
 * stack or user pointer survives suspension: all progress is in this record. */
int console_wait_work(unsigned slot,uint32_t now){
    struct waiter *w=waiters+slot;struct process *p=processes+slot;
    if(w->pid!=p->pid){console_wait_reset(slot);return 0;}
    if(!(buffered_mask&(1u<<slot)))return 0;
    for(unsigned parcel=0;parcel<CONSOLE_RENDER_PARCELS;parcel++){
      unsigned logs=0,tty=0,chosen=selected_work(&logs,&tty);
      if(chosen!=slot||render_busy)break;
      {
        unsigned size=logs?w->log_size:w->output_size,*at=logs?&w->log_at:&w->output_at;
        unsigned char *bytes=logs?w->log:w->output;unsigned n=render_span(bytes+*at,size-*at);
        render_busy=slot+1u;int result=lume_io_render(slot,tty,bytes+*at,n);render_busy=0;
        if(result<0)break;
        if(result>=0){
            *at+=n;render_turn=tty^1u;
            if(*at==size){
                heads_dirty|=1u<<tty;*at=0;
                if(logs){w->log_size=0;w->log_admitted=0;}
                else{w->output_size=0;w->output_admitted=0;p->context.regs[ARCH_REG_RESULT]=size;}
                if(!w->log_size&&!w->output_size)buffered_mask&=~(1u<<slot);
            }
        }
    }
    }
    finish_wait(slot,now);
    /* A blocked WRITE or diagnostic cannot return early. INPUT retains its
     * logical wait until its earlier diagnostic has completed too. */
    if(!(console_wait_mask&(1u<<slot))){console_wait_progress(now);return p->state!=LUME_RUNNING;}
    if(w->output_size||w->log_size)p->state=LUME_WAITING;
    else if(w->reason==INPUT||w->reason==DRAIN)p->state=LUME_WAITING;
    console_wait_progress(now);return p->state!=LUME_RUNNING;
}
void console_wait_collect(unsigned slot){
    if(!uart_queue_active()||!console_kernel_size())return;
    struct waiter *w=waiters+slot;
    if(w->log_size)panic("UART diagnostic ownership");
    unsigned n=console_kernel_take(w->log,sizeof w->log);if(!n)return;
    w->log_size=n;w->work_started=0;w->log_at=w->log_admitted=0;w->log_sequence=stamp();heads_dirty|=1u;buffered_mask|=1u<<slot;
    if(!(console_wait_mask&(1u<<slot)))hold(slot,DIAGNOSTIC);
}
int console_submit(unsigned slot,unsigned tty,const unsigned char *bytes,unsigned n){
    if(slot>=MAX_PROCESSES||tty>1||n>4096||(!bytes&&n))return -22;
    if(!n)return 0;
    console_wait_collect(slot);console_wait_progress(uptime_ms);
    unsigned admitted=0;
    if(!buffered_mask&&render_span(bytes,n)==n){
        int result=!tty&&uart_queue_active()?uart_queue_submit(bytes,n):(int)n;
        if(result!=-11){
            if(result<0)return result;
            admitted=1;result=lume_io_render(slot,tty,bytes,n);
            if(result>=0){render_turn=tty^1u;return (int)n;}
        }
    }
    struct waiter *w=waiters+slot;
    memcpy(w->output,bytes,n);w->output_size=n;w->output_at=0;w->output_admitted=admitted;
    w->work_started=0;w->tty=tty;heads_dirty|=1u<<tty;w->output_sequence=stamp();w->deadline=uptime_ms+30000u;buffered_mask|=1u<<slot;
    hold(slot,OUTPUT);console_wait_progress(uptime_ms);return (int)n;
}
int console_wait_exiting(unsigned slot,uint32_t code){
    struct waiter *w=waiters+slot;heads_dirty|=1u<<w->tty;w->output_size=w->output_at=w->output_admitted=0;
    console_wait_collect(slot);w->code=code;
    if(!w->log_size){input_mask&=~(1u<<slot);console_wait_mask&=~(1u<<slot);buffered_mask&=~(1u<<slot);w->reason=NONE;return 0;}
    hold(slot,EXITING);return 1;
}
int console_wait_terminal(unsigned slot,unsigned tty,unsigned action,uint32_t timeout){
    if(slot>=MAX_PROCESSES||tty>1)return -22;
    if(!uart_queue_active())return -95;
    if(timeout>60000u)return -22;
    struct waiter *w=waiters+slot;w->tty=tty;
    if(action==5){if(console_ready_tty(tty))return 0;if(!timeout)return -11;hold(slot,INPUT);}
    else if(action==4){if(tty)return 0;int result=uart_queue_drain();if(result!=-11||!timeout)return result;hold(slot,DRAIN);}
    else return -22;
    w->deadline=uptime_ms+timeout;return 0;
}
