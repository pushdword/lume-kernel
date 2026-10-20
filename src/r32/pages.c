/* Dynamic physical pages; metadata is serialized by the scheduler lock.
 * Reserved frames may be zeroed/filled outside it; only fully initialized
 * file pages are published. A remote RUNNING owner is never a victim.
 * A128MiB virtual window is not a reservation of128MiB physical per process. */
#include "core.h"
#include "pager_io.h"
#include "perf_io.h"
#include "fs_gate.h"
#include "slow_io.h"
#include <string.h>
#ifdef LUME_MEMORY_HOST
#define PAGE_ALIGN __declspec(align(4096))
static PAGE_ALIGN unsigned char host_ram[0x4000000u];
#else
#define PAGE_ALIGN __attribute__((aligned(4096)))
#endif
PAGE_ALIGN uint32_t user_tables[MAX_PROCESSES][USER_PAGES];
static uint32_t allocated[512];
static uint32_t pool_first,pool_pages,free_pages,next_page;
enum{F_RESERVE=1,F_RO=2,F_PIN=4,F_UNUSED=8,F_FILL=16};
struct frame_meta{uint16_t vpn;uint8_t owner,flags;};
static struct frame_meta frames[16384];
static uint16_t pinned[MAX_PROCESSES+2][128];static unsigned pin_count[MAX_PROCESSES+2];
static inline __attribute__((always_inline)) unsigned pin_context(void){unsigned c=core_id();return current[c]>=0?(unsigned)current[c]:MAX_PROCESSES+c;}
uint32_t pager_faults,pager_pageins,pager_evictions,pager_errors;
uint32_t pager_general_ro_pages,pager_reserve_fast;
struct user_memory {uint32_t pages,base,end,stack_low,low_high,reserve_count,reserve_next;uint16_t reserve[8];};
static struct user_memory memories[MAX_PROCESSES];
static unsigned char *physical(uint32_t address){
#ifdef LUME_MEMORY_HOST
    return host_ram+address-0x10000000u;
#else
    return (unsigned char *)(uintptr_t)address;
#endif
}
static void zero_page(uint32_t address){
    uint32_t start=io_perf_now();unsigned token=lume_slow_begin(LUME_SLOW_ZERO);
    memset(physical(address),0,4096);lume_slow_end(token);
    io_perf_record(PERF_ZERO,io_perf_now()-start,4096);
    pager_safe_point();
}
static uint32_t reclaim_general(unsigned slot);
void memory_start(uint32_t first,uint32_t end){
    pool_first=(first+4095u)&~4095u;end&=~4095u;
    pool_pages=end>pool_first?(end-pool_first)/4096u:0;
    if(pool_pages>16384u)pool_pages=16384u;
    free_pages=pool_pages;next_page=0;
    memset(allocated,0,sizeof allocated);memset(memories,0,sizeof memories);
    memset(user_tables,0,sizeof user_tables);
    memset(frames,0xff,sizeof frames);memset(pin_count,0,sizeof pin_count);pager_general_ro_pages=0;
}
static uint32_t page_alloc(unsigned slot){
    if(!free_pages)return reclaim_general(slot);
    unsigned cursor=next_page;
    for(unsigned n=0;n<pool_pages;n++){
        pager_checkpoint(n);unsigned page=cursor++;if(cursor==pool_pages)cursor=0;next_page=cursor;
        uint32_t bit=1u<<(page&31u);
        if(!(allocated[page/32u]&bit)){
            allocated[page/32u]|=bit;free_pages--;
            frames[page]=(struct frame_meta){0,(uint8_t)slot,F_FILL};
            uint32_t result=pool_first+page*4096u;zero_page(result);return result;
        }
    }
    return 0;
}
static uint32_t reclaim_general(unsigned slot){
    unsigned core=core_id();
    unsigned cursor=next_page;
    for(unsigned n=0;n<pool_pages;n++){
        pager_checkpoint(n);unsigned page=cursor++;if(cursor==pool_pages)cursor=0;next_page=cursor;
        struct frame_meta*f=&frames[page];unsigned owner=f->owner;
        if(owner>=MAX_PROCESSES||!(f->flags&F_RO)||(f->flags&(F_PIN|F_RESERVE|F_FILL)))continue;
        if((int)owner!=current[core]&&
            (processes[owner].state==LUME_RUNNING||current[0]==(int)owner||current[1]==(int)owner))continue;
        user_tables[owner][f->vpn]=0;memories[owner].pages--;pager_evictions++;pager_general_ro_pages--;
        f->owner=(uint8_t)slot;f->flags=F_FILL;
        uint32_t address=pool_first+page*4096u;zero_page(address);return address;
    }
    return 0;
}
static void page_free(uint32_t address){
    unsigned page=(address-pool_first)/4096u;
    if(address<pool_first||page>=pool_pages)return;
    uint32_t bit=1u<<(page&31u);
    if(frames[page].flags&F_RESERVE){frames[page].flags=F_RESERVE|F_UNUSED;return;}
    if(allocated[page/32u]&bit){
        if(frames[page].flags&F_RO)pager_general_ro_pages--;
        allocated[page/32u]&=~bit;free_pages++;
    }
    frames[page]=(struct frame_meta){0,255,0};
}
int user_reserve(unsigned slot){
    struct user_memory*m=&memories[slot];
    for(unsigned i=0;i<8;i++){
        uint32_t address=page_alloc(slot);if(!address)return 0;
        unsigned page=(address-pool_first)/4096u;
        frames[page].flags=F_RESERVE|F_UNUSED;m->reserve[m->reserve_count++]=(uint16_t)page;m->pages++;
    }
    return 1;
}
void user_unpin(void){
    unsigned core=pin_context();
    while(pin_count[core])frames[pinned[core][--pin_count[core]]].flags&=(uint8_t)~F_PIN;
}
static int pin_frame(uint32_t value){
    unsigned page=((value&~4095u)-pool_first)/4096u;
    if(!(frames[page].flags&F_RO)||(frames[page].flags&F_PIN))return 1;
    unsigned core=pin_context();if(pin_count[core]==128)return 0;
    frames[page].flags|=F_PIN;pinned[core][pin_count[core]++]=(uint16_t)page;return 1;
}
static uint32_t ro_frame(unsigned slot){
    struct user_memory*m=&memories[slot];
    for(unsigned i=0;i<m->reserve_count;i++){
        unsigned page=m->reserve[i];
        if(frames[page].flags&F_UNUSED){frames[page].flags=F_RESERVE|F_RO|F_FILL;uint32_t address=pool_first+page*4096u;zero_page(address);return address;}
    }
    /* A pool entirely occupied by anonymous pages has no general RO victim.
     * Consult this owner's eight private frames directly, instead of scanning
     * the whole physical pool twice for every fault. If general RO exists we
     * retain the ordinary FIFO policy, preserving its larger working set. */
    if(!free_pages&&!pager_general_ro_pages){
        for(unsigned n=0;n<m->reserve_count;n++){
            unsigned i=m->reserve_next++;if(m->reserve_next==m->reserve_count)m->reserve_next=0;
            unsigned page=m->reserve[i];struct frame_meta*f=&frames[page];
            if(f->owner!=slot||(f->flags&(F_RESERVE|F_RO|F_PIN|F_FILL))!=(F_RESERVE|F_RO))continue;
            user_tables[slot][f->vpn]=0;pager_evictions++;pager_reserve_fast++;f->flags|=F_FILL;
            uint32_t address=pool_first+page*4096u;zero_page(address);return address;
        }
        return 0;
    }
    uint32_t address=page_alloc(slot);
    if(address){frames[(address-pool_first)/4096u].flags=F_RO|F_FILL;m->pages++;pager_general_ro_pages++;return address;}
    /* FIFO cursor avoids re-evicting the page just faulted in when one
     * instruction needs more than one page. Never recycle a remote RUNNING
     * owner's frame; snoop invalidation alone does not cancel in-flight reads. */
    unsigned core=core_id();
    unsigned cursor=next_page;
    for(unsigned n=0;n<pool_pages;n++){
        pager_checkpoint(n);unsigned page=cursor++;if(cursor==pool_pages)cursor=0;next_page=cursor;
        struct frame_meta*f=&frames[page];unsigned owner=f->owner;
        if(owner>=MAX_PROCESSES||!(f->flags&F_RO)||(f->flags&(F_PIN|F_FILL)))continue;
        if((f->flags&F_RESERVE)&&owner!=slot)continue;
        if((int)owner!=current[core]&&
            (processes[owner].state==LUME_RUNNING||current[0]==(int)owner||current[1]==(int)owner))continue;
        user_tables[owner][f->vpn]=0; /* Clear before any physical rewrite. */
        pager_evictions++;
        if(!(f->flags&F_RESERVE)){memories[owner].pages--;m->pages++;}
        f->owner=(uint8_t)slot;f->flags=(uint8_t)((f->flags&F_RESERVE)|F_RO|F_FILL);
        address=pool_first+page*4096u;zero_page(address);return address;
    }
    return 0;
}
int user_file_fault(unsigned slot,uint32_t address,uint32_t error){
    if(slot>=MAX_PROCESSES||address<USER_BASE||address>=USER_BASE+USER_IMAGE_BYTES||(error&0x63u))return 0;
    unsigned vpn=(address-USER_BASE)/4096u;
    if(user_tables[slot][vpn])return 0;
    uint32_t permissions=file_page_permissions(slot,vpn);
    if(!permissions||((error&4u)&&!(permissions&8u)))return 0;
    if(!lume_fs_acquire())return 0;
    pager_faults++;
    uint32_t page=ro_frame(slot);if(!page){pager_errors++;return 0;}
    unsigned token=lume_slow_begin(LUME_SLOW_FAT);
    int loaded=file_page_read(slot,vpn,physical(page));lume_slow_end(token);
    if(!loaded){
        pager_errors++;
        if(!(frames[(page-pool_first)/4096u].flags&F_RESERVE))memories[slot].pages--;
        page_free(page);return 0;
    }
    struct frame_meta*f=&frames[(page-pool_first)/4096u];f->vpn=(uint16_t)vpn;f->flags&=(uint8_t)~F_FILL;
    user_tables[slot][vpn]=page|permissions;
    pager_pageins++;
    if(vpn>=memories[slot].low_high)memories[slot].low_high=vpn+1u;
    return 1;
}
static inline __attribute__((always_inline)) int return_page_ready(unsigned slot,uint32_t pc,unsigned allow_io){
    if(pc<USER_BASE||pc>=USER_BASE+USER_IMAGE_BYTES||(pc&3u))return 0;
    unsigned vpn=(pc-USER_BASE)/4096u;
    if(!user_tables[slot][vpn]){if(!allow_io)return 2;if(!user_file_fault(slot,pc,4))return 0;}
    if((user_tables[slot][vpn]&ARCH_PAGE_EXEC)!=ARCH_PAGE_EXEC||!pin_frame(user_tables[slot][vpn]))return 0;
#ifdef LUME_ARCH_R32
    (void)allow_io;return 1; /* Fixed4B instructions never cross a4KiB page. */
#else
    /* The widest instruction is12B (FARJMP: primary plus two extensions).
     * Interior PCs cannot need a second page; retain the first-page lease. */
    if((pc&4095u)<=4084u)return 1;
    /* Only valid extension-bearing encodings may need a second code page.
     * Do not read an unrelated next page for a short/invalid instruction. */
    uint32_t word=*(uint32_t*)(void*)user_pointer(slot,pc),op=word>>24,extensions=0;
    switch(op){
    case 0x03:case 0x0c:case 0x0e:case 0x12:case 0x16:case 0x2a:case 0x2b:case 0x2c:case 0x2d:case 0x2e:
        extensions=(word&0x00c1ffffu)==0x00800000u;break;
    case 0x32:extensions=(word&0x00ffffffu)==0x00800000u;break;
    case 0x41:case 0x43:case 0x45:case 0x49:extensions=(word&0x00ffffffu)==0;break;
    case 0x5b:extensions=(word&0x00e1ffffu)==0;break;
    case 0xe7:extensions=word==0xe7000000u?2:0;break;
    default:break;
    }
    uint32_t last=pc+extensions*4u;unsigned end=(last-USER_BASE)/4096u;
    if(end!=vpn&&(file_page_permissions(slot,end)&8u)){
        if(!user_tables[slot][end]){if(!allow_io)return 2;if(!user_file_fault(slot,last,4))return 0;}
        if(!pin_frame(user_tables[slot][end]))return 0;
    }
    return 1;
#endif
}
int user_return_ready(unsigned slot,uint32_t pc){return return_page_ready(slot,pc,1);}
int user_return_probe(unsigned slot,uint32_t pc){return return_page_ready(slot,pc,0);}
void user_release(unsigned slot){
    if(slot>=MAX_PROCESSES)return;
    struct user_memory *m=&memories[slot];
    /* A failed return-PC extension can leave a lease on its first page. Revoke
     * all leases for this owner before freeing frames: a delayed trap on the
     * old core must never unpin a frame since recycled by the other core. */
    for(unsigned core=0;core<MAX_PROCESSES+2;core++){
        unsigned n=0;
        while(n<pin_count[core]){
            unsigned page=pinned[core][n];
            if(frames[page].owner==slot){
                frames[page].flags&=(uint8_t)~F_PIN;
                pinned[core][n]=pinned[core][--pin_count[core]];
            }else n++;
        }
    }
    file_backing_release(slot);
    /* Small commands must not scan the entire128MiB virtual window on exit.
     * Track the highest image/heap page and committed stack independently. */
    for(unsigned page=0;page<m->low_high;page++){if(user_tables[slot][page]){
        page_free(user_tables[slot][page]&~4095u);user_tables[slot][page]=0;
    }pager_checkpoint(page);}
    if(m->stack_low)for(unsigned page=(m->stack_low-USER_BASE)/4096u;page<USER_PAGES;page++){if(user_tables[slot][page]){
        page_free(user_tables[slot][page]&~4095u);user_tables[slot][page]=0;
    }pager_checkpoint(page);}
    for(unsigned i=0;i<m->reserve_count;i++){
        unsigned page=m->reserve[i];frames[page].flags=0;page_free(pool_first+page*4096u);
    }
    memories[slot]=(struct user_memory){0};
}
int user_map_page(unsigned slot,unsigned page,uint32_t permissions){
    if(slot>=MAX_PROCESSES||page>=USER_PAGES||user_tables[slot][page])return 0;
    uint32_t address=page_alloc(slot);if(!address)return 0;
    frames[(address-pool_first)/4096u].vpn=(uint16_t)page;
    frames[(address-pool_first)/4096u].flags&=(uint8_t)~F_FILL;
    user_tables[slot][page]=address|permissions;struct user_memory *m=&memories[slot];m->pages++;
    if(page<(USER_STACK_BASE-USER_BASE)/4096u){if(page>=m->low_high)m->low_high=page+1u;}
    else if(!m->stack_low||USER_BASE+page*4096u<m->stack_low)m->stack_low=USER_BASE+page*4096u;
    return 1;
}
static void unmap(unsigned slot,unsigned page){
    uint32_t value=user_tables[slot][page];if(!value)return;
    user_tables[slot][page]=0;page_free(value&~4095u);memories[slot].pages--;
}
unsigned char *user_pointer(unsigned slot,uint32_t pointer){
    return physical(user_tables[slot][(pointer-USER_BASE)/4096u]&~4095u)+(pointer&4095u);
}
/* Read-only admission hint under queue_lock. No fault, pin or copy occurs;
 * the ordinary validator still performs every check before any effect. */
int user_resident(unsigned slot,uint32_t pointer,unsigned bytes,unsigned write){
    if(!bytes)return 1;
    if(slot>=MAX_PROCESSES||pointer<USER_BASE||pointer>=USER_END||bytes>USER_END-pointer)return 0;
    unsigned first=(pointer-USER_BASE)/4096u,last=(pointer-USER_BASE+bytes-1u)/4096u;
    unsigned bits=write?ARCH_PAGE_WRITE:ARCH_PAGE_READ;
    for(unsigned page=first;page<=last;page++)if((user_tables[slot][page]&bits)!=bits)return 0;
    return 1;
}
int user_range(unsigned slot,uint32_t pointer,unsigned bytes,unsigned write){
    if(!bytes)return 1;
    if(slot>=MAX_PROCESSES||pointer<USER_BASE||pointer>=USER_END||bytes>USER_END-pointer)return 0;
    unsigned first=(pointer-USER_BASE)/4096u,last=(pointer-USER_BASE+bytes-1u)/4096u;
    unsigned bits=write?7u:3u;
    for(unsigned page=first;page<=last;page++){
        if(!user_tables[slot][page]&&!write)user_file_fault(slot,USER_BASE+page*4096u,0);
        if((user_tables[slot][page]&bits)!=bits||(user_tables[slot][page]&ARCH_PAGE_USER)!=ARCH_PAGE_USER||!pin_frame(user_tables[slot][page]))return 0;
    }
    return 1;
}
void user_copy_from(unsigned slot,void *out,uint32_t pointer,unsigned bytes){
    unsigned char *to=out;
    while(bytes){unsigned n=4096u-(pointer&4095u);if(n>bytes)n=bytes;
        memcpy(to,user_pointer(slot,pointer),n);pointer+=n;to+=n;bytes-=n;}
}
void user_copy_to(unsigned slot,uint32_t pointer,const void *in,unsigned bytes){
    const unsigned char *from=in;
    while(bytes){unsigned n=4096u-(pointer&4095u);if(n>bytes)n=bytes;
        memcpy(user_pointer(slot,pointer),from,n);pointer+=n;from+=n;bytes-=n;}
}
void user_image_ready(unsigned slot,uint32_t end){
    struct user_memory *m=&memories[slot];m->base=m->end=(end+4095u)&~4095u;
    m->stack_low=USER_BASE+USER_STACK_FIRST*4096u;
}
int32_t user_brk(unsigned slot,uint32_t end){
    struct user_memory *m=&memories[slot];if(!end)return (int32_t)m->end;
    if(!m->base||end<m->base||end>USER_BASE+USER_IMAGE_BYTES)return -22;
    unsigned first=(m->end-USER_BASE+4095u)/4096u,last=(end-USER_BASE+4095u)/4096u;
    if(last>first){
        unsigned page=first,old_high=m->low_high;
        for(;page<last;page++)if(!user_map_page(slot,page,ARCH_PAGE_WRITE)){
            while(page>first){unmap(slot,--page);pager_checkpoint(page);}m->low_high=old_high;return -12;
        }
    }else if(last<first){for(unsigned page=last;page<first;page++){unmap(slot,page);pager_checkpoint(page);}m->low_high=last;}
    if(end<m->end&&(end&4095u)&&user_range(slot,end,1,1))
        memset(user_pointer(slot,end),0,4096u-(end&4095u));
    m->end=end;return (int32_t)end;
}
int user_stack_fault(unsigned slot,uint32_t address,uint32_t sp,uint32_t error){
    struct user_memory *m=&memories[slot];
    if((error&0x65u)||address<USER_STACK_BASE||address>=m->stack_low||
       sp<USER_STACK_BASE||sp>=USER_END||address+32u<sp)return 0;
    uint32_t old_low=m->stack_low;
    unsigned first=((address<sp?address:sp)-USER_BASE)/4096u,last=(old_low-USER_BASE)/4096u,page=first;
    for(;page<last;page++)if(!user_map_page(slot,page,ARCH_PAGE_WRITE)){
        while(page>first){unmap(slot,--page);pager_checkpoint(page);}m->stack_low=old_low;return 0;
    }
    m->stack_low=USER_BASE+first*4096u;return 1;
}
void user_memory_info(unsigned slot,struct lume_memory_info *out){
    struct user_memory *m=&memories[slot];
    *out=(struct lume_memory_info){LUME_MEMORY_VERSION,sizeof *out,4096u,pool_pages,free_pages,
        m->pages,m->base,m->end,USER_BASE+USER_IMAGE_BYTES,USER_STACK_BASE,USER_END};
}
