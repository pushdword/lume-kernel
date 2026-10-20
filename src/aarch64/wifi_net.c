/* Bounded decoders for Broadcom Ethernet events and escan BSS v109. */
#include "lume/wifi_protocol.h"
static unsigned le16(const unsigned char *p){return p[0]|(unsigned)p[1]<<8;}
static uint32_t le32(const unsigned char *p){return le16(p)|(uint32_t)le16(p+2)<<16;}
static unsigned be16(const unsigned char *p){return (unsigned)p[0]<<8|p[1];}
static uint32_t be32(const unsigned char *p){return (uint32_t)be16(p)<<16|be16(p+2);}
bool lume_wifi_event_parse(const void *data,size_t n,struct lume_wifi_event *e)
{
    const unsigned char *p=data;
    if(!p || !e || n<72 || be16(p+12)!=0x886c || be16(p+14)!=0x8001 ||
       p[18]!=0 || p[19]!=0 || p[20]!=0x10 || p[21]!=0x18 || be16(p+22)!=1 ||
       be16(p+24)!=2 || p[70]!=0 || p[71]!=0 || be32(p+44)>n-72)return false;
    *e=(struct lume_wifi_event){be32(p+28),be32(p+32),be32(p+36),be16(p+26),p+72,be32(p+44)};
    return true;
}
bool lume_wifi_bss(const void *data,size_t n,struct lume_access_point *out,size_t *used)
{
    const unsigned char *p=data;
    if(!p || !out || !used || n<124 || le32(p)!=109)return false;
    size_t bytes=le32(p+4),ie=le16(p+116),len=le32(p+120);
    if(bytes<124 || bytes>n || p[18]>32 || ie<124 || ie>bytes || len>bytes-ie)return false;
    *out=(struct lume_access_point){0};
    for(unsigned i=0;i<6;++i)out->bssid[i]=p[8+i];
    out->ssid_len=p[18];for(unsigned i=0;i<out->ssid_len;++i)out->ssid[i]=(char)p[19+i];
    out->rssi=(int16_t)le16(p+78);out->channel=p[88]?p[88]:(uint8_t)le16(p+72);
    out->security=(le16(p+16)&16)?LUME_WIFI_PRIVACY:0;
    for(size_t pos=ie;pos<ie+len;) {
        if(ie+len-pos<2 || p[pos+1]>ie+len-pos-2)return false;
        size_t size=p[pos+1];
        if(p[pos]==48 && size>=2)out->security|=LUME_WIFI_RSN;
        if(p[pos]==221 && size>=4 && p[pos+2]==0 && p[pos+3]==0x50 && p[pos+4]==0xf2 && p[pos+5]==1)out->security|=LUME_WIFI_WPA;
        pos+=size+2;
    }
    *used=bytes;return true;
}
size_t lume_wifi_data_frame(void *output,size_t cap,uint8_t sequence,const void *data,size_t n)
{
    /* Two pad bytes align the Ethernet IP payload for the firmware DMA. */
    unsigned char *p=output;const unsigned char *q=data;size_t size=18+n,pad=(size+3)&~(size_t)3;
    if(!p || !q || n<14 || n>1514 || pad>cap)return 0;
    for(size_t i=0;i<pad;++i)p[i]=0;
    p[0]=(unsigned char)size;p[1]=(unsigned char)(size>>8);p[2]=(unsigned char)~size;p[3]=(unsigned char)(~size>>8);
    p[4]=sequence;p[5]=2;p[7]=14;p[14]=0x20;
    for(size_t i=0;i<n;++i)p[18+i]=q[i];return pad;
}
