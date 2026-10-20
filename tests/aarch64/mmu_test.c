#include "lume/mmu.h"
#include <stdio.h>
#include <stdlib.h>
_Alignas(4096) unsigned char __user_code_start[4096];
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"MMU FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void)
{
    struct lume_fdt fdt = {.ram_count=2,.ram={{0x80000,0x3fb80000},{0x40000000,0x1c0000000}}};
    struct lume_layout layout = {0x200000,0x204000,0x207000,0x300000,0x2ef000};
    CHECK(lume_mmu_tables(&fdt,&layout));
    uint64_t pa, root = lume_mmu_root();
    CHECK(!lume_mmu_translate(root,0,false,false,false,&pa));
    CHECK(!lume_mmu_translate(root,0x80000,false,false,false,&pa));
    CHECK(lume_mmu_translate(root,0x200000,false,false,true,&pa) && pa==0x200000);
    CHECK(!lume_mmu_translate(root,0x200000,true,false,false,&pa));
    CHECK(!lume_mmu_translate(root,0x200000,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x204000,false,false,true,&pa));
    CHECK(!lume_mmu_translate(root,0x204000,true,false,false,&pa));
    CHECK(lume_mmu_translate(root,0x207123,true,false,false,&pa) && pa==0x207123);
    CHECK(!lume_mmu_translate(root,0x207000,false,false,true,&pa));
    CHECK(!lume_mmu_translate(root,layout.guard,true,false,false,&pa));
    CHECK(!lume_mmu_translate(root,0x300000,false,false,false,&pa));
    CHECK(lume_mmu_translate(root,0x100001234,true,false,false,&pa) && pa==0x100001234);
    CHECK(lume_mmu_translate(root,0x1fffffff8,true,false,false,&pa) && pa==0x1fffffff8);
    CHECK(!lume_mmu_translate(root,0x200000000,false,false,false,&pa));
    CHECK(lume_mmu_translate(root,0x107fff9000,true,false,false,&pa) && pa==0x107fff9000);
    CHECK(!lume_mmu_translate(root,0x107fff9000,false,false,true,&pa));
    CHECK(lume_mmu_translate(root,0x1c00030000,true,false,false,&pa) && pa==0x1c00030000);
    CHECK(!lume_mmu_translate(root,0x1c00030000,false,true,false,&pa));
    CHECK(lume_mmu_translate(root,0x1000124034,true,false,false,&pa) && pa==0x1000124034);
    CHECK(!lume_mmu_translate(root,0x1000124034,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x1000124034,false,false,true,&pa));
    CHECK(!lume_mmu_translate(root,0x1000200000,false,false,false,&pa));
    CHECK(lume_mmu_translate(root,0x1000fff000,true,false,false,&pa) && pa==0x1000fff000);
    CHECK(!lume_mmu_translate(root,0x1000fff000,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x1000fff000,false,false,true,&pa));
    CHECK(lume_mmu_translate(root,0x1001100000,true,false,false,&pa) && pa==0x1001100000);
    CHECK(!lume_mmu_translate(root,0x1001100000,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x1001100000,false,false,true,&pa));
    CHECK(lume_mmu_translate(root,0x107d508500,true,false,false,&pa) && pa==0x107d508500);
    CHECK(!lume_mmu_translate(root,0x107d508500,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x107d504100,false,false,true,&pa));
    CHECK(lume_mmu_translate(root,0x107d208000,true,false,false,&pa) && pa==0x107d208000);
    CHECK(!lume_mmu_translate(root,0x107d208000,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x107d208000,false,false,true,&pa));
    CHECK(lume_mmu_translate(root,0x1c00200000,true,false,false,&pa) && pa==0x1c00200000);
    CHECK(lume_mmu_translate(root,0x1c00300000,true,false,false,&pa) && pa==0x1c00300000);
    CHECK(!lume_mmu_translate(root,0x1c00300000,false,true,false,&pa));
    CHECK(!lume_mmu_translate(root,0x1c00300000,false,false,true,&pa));
    CHECK(!lume_mmu_translate(root,0x1c00400000,false,false,false,&pa));
    uint64_t stacks[4]={0x100010000,0x100011000,0x100012000,0x100013000};
    CHECK(lume_mmu_user_root(0,0x100000000,stacks));
    CHECK(lume_mmu_user_root(1,0x100001000,stacks));
    CHECK(lume_mmu_user_root(2,0x100002000,stacks));
    CHECK(!lume_mmu_user_root(3,0x100000000,stacks));
    CHECK(lume_mmu_translate(lume_mmu_task_root(2),LUME_USER_DATA,true,true,false,&pa) && pa==0x100002000);
    CHECK(lume_mmu_task_root(3)==0);
    CHECK(!lume_mmu_user_root(0,0x100000001,stacks));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_BASE,false,true,true,&pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_BASE,true,true,false,&pa));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_DATA,true,true,false,&pa) && pa==0x100000000);
    CHECK(lume_mmu_translate(lume_mmu_task_root(1),LUME_USER_DATA,true,true,false,&pa) && pa==0x100001000);
    CHECK(!lume_mmu_translate(root,LUME_USER_DATA,false,true,false,&pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_DATA,false,true,true,&pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_STACK-4096,false,true,false,&pa));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_STACK+16383,true,true,false,&pa) && pa==0x100013fff);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0),LUME_USER_STACK+16384,false,true,false,&pa));
    layout.end=0x400001;
    CHECK(!lume_mmu_tables(&fdt,&layout));
    printf("PASS_MMU_TABLES checks=%u\n",checks);
}
