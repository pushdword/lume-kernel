/* SDIO discovery and CYW43455 function-1 register access. See WIRELESS.md.
 * Byte/block PIO transport is supplied by the platform backend. */
#include "lume/sdio.h"
#define R5_ERRORS 0xcf00u

bool lume_sdio_direct(const struct lume_sdio_bus *bus, bool write, unsigned fn,
                      uint32_t address, unsigned char *byte)
{
    if(!bus || !bus->command || !byte || fn>7 || address>0x1ffff)return false;
    uint32_t argument=(write?0x80000000u:0)|(fn<<28)|(address<<9)|(write?*byte:0);
    uint32_t response;
    if(!bus->command(52,argument,LUME_SDIO_RESPONSE|LUME_SDIO_CHECKS,&response) ||
       (response&R5_ERRORS))return false;
    if(!write)*byte=(unsigned char)response;
    return true;
}
static bool read_byte(const struct lume_sdio_bus *b,unsigned fn,uint32_t a,unsigned char *v)
{ return lume_sdio_direct(b,false,fn,a,v); }
static bool write_byte(const struct lume_sdio_bus *b,unsigned fn,uint32_t a,unsigned char v)
{ return lume_sdio_direct(b,true,fn,a,&v); }

static bool cis_identity(const struct lume_sdio_bus *b,uint16_t *vendor,uint16_t *device)
{
    uint32_t pointer=0;
    for(unsigned i=0;i<3;++i) {
        unsigned char v;if(!read_byte(b,0,9+i,&v))return false;
        pointer|=(uint32_t)v<<(8*i);
    }
    if(pointer<0x1000 || pointer>0x1ffff)return false;
    unsigned consumed=0,tuples=0;bool found=false;
    while(consumed<1024 && tuples++<64 && pointer<=0x1ffff) {
        unsigned char code,link;
        if(!read_byte(b,0,pointer++,&code))return false;
        ++consumed;
        if(code==0xff)return found;
        if(code==0)continue;
        if(pointer>0x1ffff || !read_byte(b,0,pointer++,&link))return false;
        ++consumed;
        if(link==0xff || link>1024-consumed || link>0x20000-pointer)return false;
        if(code==0x20) { /* CISTPL_MANFID, two little-endian 16-bit identifiers. */
            unsigned char id[4];
            if(found || link!=4)return false;
            for(unsigned i=0;i<4;++i)if(!read_byte(b,0,pointer+i,&id[i]))return false;
            *vendor=(uint16_t)(id[0]|(unsigned)id[1]<<8);
            *device=(uint16_t)(id[2]|(unsigned)id[3]<<8);found=true;
        }
        pointer+=link;consumed+=link;
    }
    return false;
}
bool lume_sdio_backplane_read(const struct lume_sdio_bus *b,uint32_t address,uint32_t *out)
{
    if(!b || !b->read53 || !out || (address&3))return false;
    uint32_t window=address&~0x7fffu;
    for(unsigned i=0;i<3;++i)
        if(!write_byte(b,1,0x1000a+i,(unsigned char)(window>>(8+8*i))))return false;
    uint32_t offset=(address&0x7fff)|0x8000;
    uint32_t argument=(1u<<28)|(1u<<26)|(offset<<9)|4;
    unsigned char data[4];
    if(!b->read53(argument,data,4))return false;
    *out=(uint32_t)data[0]|(uint32_t)data[1]<<8|(uint32_t)data[2]<<16|(uint32_t)data[3]<<24;
    return true;
}
bool lume_sdio_memory(const struct lume_sdio_bus *b,bool write,uint32_t address,void *data,size_t bytes)
{
    if(!b || !b->command || !b->read53 || (write && !b->write53) || !data ||
       !bytes || ((address|bytes)&3) || bytes>UINT32_MAX-address)return false;
    unsigned char *p=data;uint32_t selected=UINT32_MAX;
    while(bytes) {
        uint32_t window=address&~0x7fffu;
        if(window!=selected) {
            for(unsigned i=0;i<3;++i)
                if(!write_byte(b,1,0x1000a+i,(unsigned char)(window>>(8+8*i))))return false;
            selected=window;
        }
        size_t count=0x8000-(address&0x7fff);
        if(count>bytes)count=bytes;
        if(count>512)count=512;
        /* Function 1 advertises a 64-byte block. Keep larger operations in
         * block mode, then use byte mode only for the final <64 bytes. */
        bool blocks=count>=64;
        if(blocks)count&=~(size_t)63;
        uint32_t offset=(address&0x7fff)|0x8000;
        uint32_t arg=(write?0x80000000u:0)|(1u<<28)|(blocks?1u<<27:0)|(1u<<26)|
                     (offset<<9)|(uint32_t)(blocks?count/64:count&511);
        if(write?!b->write53(arg,p,count):!b->read53(arg,p,count))return false;
        p+=count;address+=(uint32_t)count;bytes-=count;
    }
    return true;
}
bool lume_sdio_backplane_write(const struct lume_sdio_bus *b,uint32_t address,uint32_t value)
{
    unsigned char data[4];
    for(unsigned i=0;i<4;++i)data[i]=(unsigned char)(value>>(i*8));
    return lume_sdio_memory(b,true,address,data,4);
}
bool lume_sdio_identify(const struct lume_sdio_bus *b,struct lume_sdio_identity *out)
{
    if(!b || !b->command || !b->read53 || !b->delay_us || !out)return false;
    *out=(struct lume_sdio_identity){0};uint32_t r;
    if(!b->command(0,0,0,0) || !b->command(5,0,LUME_SDIO_RESPONSE,&r))return false;
    out->functions=(r>>28)&7;
    if(!out->functions || !(r&0x300000))return false;
    bool ready=false;
    for(unsigned n=0;n<200;++n) {
        if(!b->command(5,0x300000,LUME_SDIO_RESPONSE,&r))return false;
        if(r&0x80000000u){ready=true;break;}
        b->delay_us(10000);
    }
    if(!ready)return false;
    out->ocr=r;
    if(!b->command(3,0,LUME_SDIO_RESPONSE|LUME_SDIO_CHECKS,&r) || (r&0xe000))return false;
    out->rca=r&0xffff0000u;
    if(!out->rca || !b->command(7,out->rca,(3u<<16)|LUME_SDIO_CHECKS,&r) ||
       (r&0xfdffe008u))return false;
    if(!read_byte(b,0,0,&out->cccr) || !read_byte(b,0,1,&out->version) ||
       !read_byte(b,0,8,&out->capability) || !cis_identity(b,&out->vendor,&out->device))return false;
    for(unsigned i=1;i<=out->functions;++i)
        if(!read_byte(b,0,i*0x100,&out->interface[i-1]))return false;
    /* Backplane registers below are specific to this chip family. */
    if(out->vendor!=0x02d0 || out->device!=0x4345)return false;
    unsigned char enabled;
    if(!write_byte(b,0,4,0) || !read_byte(b,0,2,&enabled) ||
       !write_byte(b,0,2,(unsigned char)(enabled|2)))return false;
    ready=false;
    for(unsigned n=0;n<100;++n) {
        unsigned char v;if(!read_byte(b,0,3,&v))return false;
        if(v&2){ready=true;break;}b->delay_us(10000);
    }
    if(!ready || !write_byte(b,0,0x110,64) || !write_byte(b,0,0x111,0) ||
       !write_byte(b,1,0x1000e,0x28))return false; /* ALP request, hardware requests off */
    ready=false;
    for(unsigned n=0;n<100;++n) {
        if(!read_byte(b,1,0x1000e,&out->clock_csr))return false;
        if(out->clock_csr&0x40){ready=true;break;}b->delay_us(1000);
    }
    if(!ready || !write_byte(b,1,0x1000e,0x21))return false;
    if(!lume_sdio_backplane_read(b,0x18000000,&out->signature))return false;
    return (out->signature&0xffff)==0x4345 && ((out->signature>>16)&15)==6;
}
