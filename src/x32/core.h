#ifndef LUME_CORE_H
#define LUME_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <api.h>
#include <perf.h>
#define MAX_PROCESSES 8u
#define USER_BASE 0x40000000u
#define USER_BYTES 0x8000000u
#define USER_PAGES (USER_BYTES/4096u)
#define USER_TABLES (USER_PAGES/1024u)
#define USER_STACK_BYTES 0x100000u
#define USER_STACK_FIRST (USER_PAGES-6u)
#define USER_STACK_BASE (USER_BASE+USER_BYTES-USER_STACK_BYTES)
#define USER_IMAGE_BYTES (USER_BYTES-USER_STACK_BYTES-8192u)
#define USER_END (USER_BASE+USER_BYTES)
#define CORE_STACK0 0x13ed8000u
#define CORE_STACK1 0x13edc000u
struct return_frame {uint32_t pc,cs,flags,sp,ss,ring,vector,error;};
struct context {uint32_t regs[32],segments[4];struct return_frame frame;};
struct process {struct context context;uint32_t pid,parent,uid,gid,state,runs,cores,exit_code;
    char name[24],cwd[128];uint32_t wait_pid,wait_status;};
extern struct process processes[MAX_PROCESSES];
extern uint32_t user_tables[MAX_PROCESSES][USER_PAGES],roots[MAX_PROCESSES+1][1024];
void memory_start(uint32_t,uint32_t);
void user_release(unsigned);
int user_map_page(unsigned,unsigned,uint32_t);
unsigned char *user_pointer(unsigned,uint32_t);
int user_range(unsigned,uint32_t,unsigned,unsigned);
int user_resident(unsigned,uint32_t,unsigned,unsigned);
void user_copy_from(unsigned,void *,uint32_t,unsigned);
void user_copy_to(unsigned,uint32_t,const void *,unsigned);
void user_image_ready(unsigned,uint32_t);
int32_t user_brk(unsigned,uint32_t);
int user_stack_fault(unsigned,uint32_t,uint32_t,uint32_t);
int user_file_fault(unsigned,uint32_t,uint32_t);
int user_return_ready(unsigned,uint32_t);
int user_return_probe(unsigned,uint32_t);
int user_reserve(unsigned);
void user_unpin(void);
uint32_t file_page_permissions(unsigned,uint32_t);
int file_page_read(unsigned,uint32_t,unsigned char *);
void file_backing_release(unsigned);
void user_memory_info(unsigned,struct lume_memory_info *);
extern uint32_t clock_hz,quantum_ms,uptime_ms,cpu_forms;
extern int current[2];
extern volatile uint32_t secondary_ready;
void kputs(const char *);void kputc(unsigned);void knumber(uint32_t);void khex(uint32_t);void console_start(void);
void console_write(const unsigned char *,unsigned);
int console_getchar(void);int console_ready(void);unsigned console_geometry(void);
int console_video(unsigned,unsigned,const unsigned char *);
_Noreturn void panic(const char *);
void lock_kernel(void);void unlock_kernel(void);
/* Kernel/FS hooks run under the existing queue lock, or before SMP starts. */
enum { PERF_LOAD,PERF_SD_READ,PERF_SD_WRITE,PERF_CACHE_HIT,PERF_CACHE_MISS,
       PERF_ZERO,PERF_ELF_READ,PERF_ELF_COPY };
uint32_t lume_perf_counter(void);
uint32_t lume_work_counter(void);
void lume_perf_record(unsigned,uint32_t,uint32_t);
extern volatile uint32_t lume_perf_enabled;
void mmu_start(void);void cpu_start(unsigned);void arch_root(uint32_t);
unsigned core_id(void);
_Noreturn void x32_run_context(struct context *);
extern void vector_entry(void),secondary_entry(void),idle_loop(void);
struct context *dispatch(struct context *);
int spawn(const char *,uint32_t);
void filesystem_start(void);int file_load_process(unsigned,const char *,uint32_t *);
int fs_list(const char *,uint32_t,struct lume_directory_entry *);
int fs_list_owned(unsigned,const char *,uint32_t,struct lume_directory_entry *);
int fs_read(const char *,uint32_t,unsigned char *,unsigned);
int fs_path(const char *,const char *,char [128]);
int fs_open(unsigned,const char *,unsigned);int fs_close(unsigned,int);
int fs_fdread(unsigned,int,void *,unsigned);int fs_fdwrite(unsigned,int,const void *,unsigned);
int fs_seek(unsigned,int,int32_t,unsigned);int fs_sync(unsigned,int);
int fs_stat(const char *,struct lume_stat *);int fs_mkdir(const char *);
int fs_unlink(const char *);int fs_rename(const char *,const char *);
int fs_rmdir(const char *);
int fs_fstat(unsigned,int,struct lume_stat *);int fs_truncate_here(unsigned,int);
void fs_process_exit(unsigned);
int fs_object_open(const char *,unsigned);
void fs_directory_exit(unsigned);
void fs_object_exit_failure(void);
#endif
