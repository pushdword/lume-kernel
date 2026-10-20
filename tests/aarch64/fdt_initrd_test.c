#include "lume/early.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define T(x) do{assert(x);++checks;}while(0)
static void word(unsigned char *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static unsigned make(unsigned char blob[512],unsigned a,unsigned b,uint64_t start,uint64_t end)
{
    memset(blob,0,512);unsigned at=56;
    word(blob+at,1);at+=8; /* root node and empty name */
    word(blob+at,1);at+=4;memcpy(blob+at,"chosen",7);at+=8;
    const char names[]="linux,initrd-start\0linux,initrd-end\0";
    if(a){word(blob+at,3);word(blob+at+4,a);word(blob+at+8,0);at+=12;for(unsigned i=0;i<a;++i)blob[at+i]=(unsigned char)(start>>((a-1-i)*8));at+=(a+3)&~3u;}
    if(b){word(blob+at,3);word(blob+at+4,b);word(blob+at+8,19);at+=12;for(unsigned i=0;i<b;++i)blob[at+i]=(unsigned char)(end>>((b-1-i)*8));at+=(b+3)&~3u;}
    word(blob+at,2);word(blob+at+4,2);word(blob+at+8,9);at+=12;
    unsigned strings=at;memcpy(blob+at,names,sizeof(names));at+=sizeof(names);
    word(blob,0xd00dfeed);word(blob+4,at);word(blob+8,56);word(blob+12,strings);word(blob+16,40);
    word(blob+20,17);word(blob+24,16);word(blob+32,sizeof(names));word(blob+36,strings-56);return at;
}
int main(void)
{
    unsigned char data[512];struct lume_fdt f;
    for(unsigned a=4;a<=8;a+=4)for(unsigned b=4;b<=8;b+=4){unsigned n=make(data,a,b,0x8000000,0xa200000);T(lume_fdt_read(data,n,&f));T(f.initrd_start==0x8000000 && f.initrd_end==0xa200000);for(unsigned k=0;k<n;++k)T(!lume_fdt_read(data,k,&f));}
    unsigned n=make(data,4,0,0x8000000,0);T(!lume_fdt_read(data,n,&f));
    n=make(data,0,4,0,0xa200000);T(!lume_fdt_read(data,n,&f));
    n=make(data,3,4,0x800000,0xa200000);T(!lume_fdt_read(data,n,&f));
    n=make(data,8,8,0x8000000,0x7ffffff);T(!lume_fdt_read(data,n,&f));
    n=make(data,8,8,0x8000000,0xc000001);T(!lume_fdt_read(data,n,&f));
    n=make(data,8,8,0x1ffffff00,0x200000100);T(!lume_fdt_read(data,n,&f));
    printf("PASS_FDT_INITRD checks=%u\n",checks);
}
