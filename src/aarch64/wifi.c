/* Bounded wire-format helpers. Register/protocol references: WIRELESS.md. */
#include "lume/wifi.h"
static uint32_t le32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(n>>(8*i));}
static unsigned le16(const unsigned char *p){return p[0]|(unsigned)p[1]<<8;}
uint32_t lume_wifi_crc32(uint32_t crc,const void *data,size_t bytes)
{
    const unsigned char *p=data;crc=~crc;
    for(size_t n=0;n<bytes;++n) {
        crc^=p[n];
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
bool lume_wifi_erom(const unsigned char *data,size_t bytes,struct lume_wifi_cores *out)
{
    if(!data || !out || bytes<8 || bytes>4096 || (bytes&3))return false;
    *out=(struct lume_wifi_cores){0};unsigned core=0,revision=0,wrapper_type=0;
    bool ended=false;unsigned seen=0,components=0;
    for(size_t pos=0;pos<bytes;pos+=4) {
        uint32_t word=le32(data+pos);
        if((word&15)==15){ended=true;break;}
        if((word&15)==1) {
            if(pos+8>bytes || (le32(data+pos+4)&15)!=1)return false;
            core=(word>>8)&0xfff;
            uint32_t info=le32(data+pos+4);
            revision=info>>24;wrapper_type=(info&0x7c000)?3:2;pos+=4;
            unsigned bit=core==0x800?1:core==0x83e?2:core==0x812?4:core==0x829?8:0;
            if(bit && (components&bit))return false;
            components|=bit;
        } else if((word&7)==5) {
            if(!core)return false;
            if(word&8) {if(pos+4>=bytes)return false;pos+=4;continue;}
            unsigned size=(word>>4)&3,type=(word>>6)&3;
            if(size==3) {
                if(pos+4>=bytes)return false;
                uint32_t extra=le32(data+pos+4);pos+=4;
                if(extra&8){if(pos+4>=bytes)return false;pos+=4;}
                continue;
            }
            if(size>1)continue;
            uint32_t address=word&~0xfffu;
            if(address<0x18000000 || address>=0x18200000)return false;
            uint32_t *target=0;unsigned bit=0;
            if(core==0x800 && type==0){target=&out->common;bit=1;}
            if(core==0x83e && type==0){target=&out->arm;bit=2;out->arm_revision=revision;}
            if(core==0x83e && type==wrapper_type){target=&out->arm_wrap;bit=4;}
            if(core==0x812 && type==wrapper_type){target=&out->d11_wrap;bit=8;}
            if(core==0x829 && type==0){target=&out->sdio;bit=16;out->sdio_revision=revision;}
            if(target) {
                /* A component can expose several slave regions. The first
                 * regular region and its primary wrapper are the core regs. */
                if(!(seen&bit)){*target=address;seen|=bit;}
            }
        }
    }
    return ended && seen==31 && out->common==0x18000000;
}
size_t lume_wifi_nvram(const unsigned char *in,size_t bytes,unsigned char *out,size_t cap)
{
    if(!in || !out || !bytes || bytes>4096)return 0;
    size_t pos=0,used=0;unsigned lines=0;
    while(pos<bytes) {
        size_t begin=pos,end;
        while(pos<bytes && in[pos]!='\n')++pos;
        end=pos;if(pos<bytes)++pos;
        while(begin<end && (in[begin]==' ' || in[begin]=='\t' || in[begin]=='\r'))++begin;
        for(size_t n=begin;n<end;++n) {
            if(in[n]=='#'){end=n;break;}
            if((in[n]<32 && in[n]!='\r' && in[n]!='\t') || in[n]>126)return 0;
        }
        while(end>begin && (in[end-1]==' ' || in[end-1]=='\t' || in[end-1]=='\r'))--end;
        if(end==begin)continue;
        bool equals=false;
        for(size_t n=begin+1;n<end;++n)if(in[n]=='=')equals=true;
        if(!equals || end-begin+1>cap-used)return 0;
        while(begin<end)out[used++]=in[begin++];
        out[used++]=0;++lines;
    }
    if(!lines || used>=cap)return 0;
    out[used++]=0;
    while(used&3){if(used>=cap)return 0;out[used++]=0;}
    return used;
}
size_t lume_wifi_control_request(unsigned char *out,size_t cap,unsigned char sequence,uint16_t id,
                                uint32_t command,bool set,const void *input,size_t bytes,size_t response)
{
    if(!out || (bytes && !input) || bytes>1500 || response>1500)return 0;
    size_t payload=bytes>response?bytes:response,total=28+payload,padded=(total+3)&~(size_t)3;
    if(padded>cap)return 0;
    for(size_t n=0;n<padded;++n)out[n]=0;
    out[0]=(unsigned char)total;out[1]=(unsigned char)(total>>8);
    out[2]=(unsigned char)~total;out[3]=(unsigned char)(~total>>8);
    out[4]=sequence;out[7]=12;
    put32(out+12,command);put32(out+16,(uint32_t)payload);
    put32(out+20,(uint32_t)id<<16|(set?2u:0));
    const unsigned char *p=input;for(size_t n=0;n<bytes;++n)out[28+n]=p[n];
    return padded;
}
bool lume_wifi_frame_header(const unsigned char *p,size_t bytes,struct lume_wifi_frame *out)
{
    if(!p || !out || bytes<12)return false;
    unsigned len=le16(p),check=le16(p+2);
    if((len^check)!=65535 || len<12 || len>4096 || p[7]<12 || p[7]>len || (p[5]&0xf0))return false;
    *out=(struct lume_wifi_frame){p[4],(unsigned char)(p[5]&15),p[9],p[8],len,p[7]};
    return true;
}
int lume_wifi_control_reply(const unsigned char *p,size_t bytes,uint16_t id,uint32_t command,
                            void *out,size_t cap,size_t *got,int32_t *status)
{
    struct lume_wifi_frame h;
    if(!got || !status || !lume_wifi_frame_header(p,bytes,&h) || bytes<h.length)return -1;
    *got=0;*status=0;
    if(h.length==h.offset)return 0; /* SDPCM credit/flow-only frame. */
    if(h.channel!=0)return 0;
    if(h.length-h.offset<16)return -1;
    const unsigned char *q=p+h.offset;
    uint32_t flags=le32(q+8),len=le32(q+4);
    if((flags>>16)!=id)return 0;
    if(le32(q)!=command || (flags&0xfffcu))return -1;
    *status=(int32_t)le32(q+12);
    if((flags&1) || *status)return -2;
    if(len>h.length-h.offset-16 || len>cap || (len && !out))return -1;
    unsigned char *dst=out;for(size_t n=0;n<len;++n)dst[n]=q[16+n];
    *got=len;return 1;
}
bool lume_wifi_tx_allowed(unsigned char sequence,unsigned char credit,unsigned char flow,bool hardware_flow)
{
    unsigned available=(unsigned char)(credit-sequence);
    return available && available<=64 && !flow && !hardware_flow;
}
