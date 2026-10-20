#include "lume/wpa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,entropy_calls;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"WPA check %u line %u\n",checks,__LINE__);exit(1);}}while(0)
static bool entropy(void *p,size_t n){++entropy_calls;for(size_t i=0;i<n;++i)((unsigned char *)p)[i]=(unsigned char)(64+i);return true;}
int main(int argc,char **argv)
{
    CHECK(argc==2);FILE *f=fopen(argv[1],"rb");CHECK(f);unsigned char blob[2048];size_t bytes=fread(blob,1,sizeof(blob),f);fclose(f);
    CHECK(bytes>4 && !memcmp(blob,"WPA2",4));const unsigned char *record[8];size_t lengths[8],pos=4;
    for(unsigned i=0;i<8;++i){CHECK(pos+4<=bytes);lengths[i]=blob[pos]|(unsigned)blob[pos+1]<<8|(unsigned)blob[pos+2]<<16|(unsigned)blob[pos+3]<<24;pos+=4;CHECK(lengths[i]<=bytes-pos);record[i]=blob+pos;pos+=lengths[i];}CHECK(pos==bytes);
    const unsigned char mac[6]={2,0,0,0,0,2};struct lume_wpa initial,s;struct lume_wpa_action a;
    CHECK(lume_wpa_begin(&initial,mac,"IEEE",4,"password",8,entropy));CHECK(!memcmp(initial.pmk,record[7],32));
    unsigned char rsn[128]={0};memcpy(rsn,lume_wpa_rsn,22);s=initial;
    for(size_t n=0;n<22;++n)CHECK(!lume_wpa_set_rsn(&s,rsn,n));
    CHECK(lume_wpa_set_rsn(&s,rsn,22));rsn[20]=12;CHECK(lume_wpa_set_rsn(&s,rsn,22) && s.rsn[20]==12);
    rsn[20]=0x40;CHECK(!lume_wpa_set_rsn(&s,rsn,22));rsn[20]=0;
    for(unsigned i=0;i<20;++i){s=initial;rsn[i]^=1;CHECK(!lume_wpa_set_rsn(&s,rsn,22));rsn[i]^=1;}
    for(size_t n=0;n<lengths[0];++n){s=initial;CHECK(lume_wpa_input(&s,record[0],n,&a)!=1);}
    s=initial;CHECK(lume_wpa_input(&s,record[0],lengths[0],&a)==1 && a.bytes==lengths[1] && !memcmp(a.frame,record[1],a.bytes));
    CHECK(!memcmp(s.ptk,record[6],64) && s.seen && !s.authorized && !a.pairwise && !a.group);
    unsigned calls=entropy_calls;CHECK(lume_wpa_input(&s,record[0],lengths[0],&a)==1 && entropy_calls==calls);
    struct lume_wpa after_m1=s;
    for(size_t n=0;n<lengths[2];++n){s=after_m1;CHECK(lume_wpa_input(&s,record[2],n,&a)!=1 && !s.authorized);}
    unsigned char mutant[512];memcpy(mutant,record[2],lengths[2]);
    for(size_t i=14;i<lengths[2];++i){s=after_m1;mutant[i]^=1;CHECK(lume_wpa_input(&s,mutant,lengths[2],&a)!=1 && !s.authorized);mutant[i]^=1;}
    s=after_m1;CHECK(lume_wpa_input(&s,record[2],lengths[2],&a)==1);
    CHECK(s.authorized && a.authorize && a.pairwise && a.group && a.gtk_id==1 && a.bytes==lengths[3] && !memcmp(a.frame,record[3],a.bytes));
    CHECK(!memcmp(a.tk,s.ptk+32,16));for(unsigned i=0;i<16;++i)CHECK(a.gtk[i]==128+i);
    CHECK(lume_wpa_input(&s,record[2],lengths[2],&a)==1 && !a.pairwise && !a.group && !a.authorize);
    CHECK(lume_wpa_input(&s,record[0],lengths[0],&a)==-1);
    CHECK(lume_wpa_input(&s,record[4],lengths[4],&a)==1 && a.group && !a.pairwise && a.gtk_id==2 && a.bytes==lengths[5] && !memcmp(a.frame,record[5],a.bytes));
    CHECK(lume_wpa_input(&s,record[4],lengths[4],&a)==1 && !a.group && !a.pairwise);
    CHECK(lume_wpa_input(&s,record[2],lengths[2],&a)==-1);
    unsigned char key[16],wrapped[24]={0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5},plain[32];
    for(unsigned i=0;i<16;++i)key[i]=(unsigned char)i;size_t n=sizeof(plain);
    CHECK(lume_wpa_unwrap(key,wrapped,24,plain,&n) && n==16);for(unsigned i=0;i<16;++i)CHECK(plain[i]==i*17);
    for(unsigned i=0;i<24;++i){wrapped[i]^=1;n=sizeof(plain);CHECK(!lume_wpa_unwrap(key,wrapped,24,plain,&n));wrapped[i]^=1;}
    printf("PASS WPA2 PMK PTK MIC M1-M4 GTK REPLAY UNWRAP %u checks\n",checks);return 0;
}
