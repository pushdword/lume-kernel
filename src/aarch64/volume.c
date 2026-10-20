#include "lume/storage.h"
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
bool lume_volume_select(const unsigned char p[512],uint64_t sectors,struct lume_volume *out) {
    *out=(struct lume_volume){0};
    if(p[510]!=0x55 || p[511]!=0xaa || u32(p+440)!=0xb8d65c68 || sectors!=121503744)return false;
    if(p[450]!=0x0c || u32(p+454)!=16384 || u32(p+458)!=1048576 ||
       p[466]!=0x83 || u32(p+470)!=1064960 || u32(p+474)!=86884352 ||
       p[478]!=0 || p[482]!=0x0c || u32(p+486)!=87949312 || u32(p+490)!=33554432)return false;
    for(unsigned i=494;i<510;++i)if(p[i])return false;
    *out=(struct lume_volume){87949312,33554432};return true;
}
