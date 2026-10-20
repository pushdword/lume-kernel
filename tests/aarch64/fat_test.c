#include "lume/fat.h"
#include "lume/wifi.h"
#include "lume/wifi_assets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,files,dirs;
#define T(x) do{assert(x);++checks;}while(0)
static bool visit(const char *p,struct lume_fat_file file,void *context)
{
    struct lume_fat *fat=context;
    if(file.directory){++dirs;return true;}
    ++files;
    if(!strcmp(p,"etc/programs")){char text[64]={0};T(file.size==28);T(lume_fat_read(fat,file,0,text,file.size)==0);T(!strcmp(text,"one two\nthree\nfour five six\n"));}
    else if(!strncmp(p,"lib/firmware/",13)) {
        uint32_t expected=0;size_t bytes=0;
        if(!strcmp(p+13,"wifi.bin")){expected=LUME_WIFI_FIRMWARE_CRC32;bytes=LUME_WIFI_FIRMWARE_BYTES;}
        else if(!strcmp(p+13,"wifi.txt")){expected=LUME_WIFI_NVRAM_CRC32;bytes=LUME_WIFI_NVRAM_BYTES;}
        else if(!strcmp(p+13,"wifi.clm")){expected=LUME_WIFI_CLM_CRC32;bytes=LUME_WIFI_CLM_BYTES;}
        else T(!strcmp(p+13,"license"));
        if(bytes)T(file.size==bytes);
        unsigned char chunk[499];uint32_t crc=0;
        for(size_t offset=0;offset<file.size;) {
            size_t count=file.size-offset;if(count>sizeof(chunk))count=sizeof(chunk);
            T(lume_fat_read(fat,file,(uint32_t)offset,chunk,count)==0);
            crc=lume_wifi_crc32(crc,chunk,count);offset+=count;
        }
        if(bytes)T(crc==expected);
    } else {
        unsigned char header[64];T(file.size>=64 && lume_fat_read(fat,file,0,header,sizeof(header))==0);
        T(!memcmp(header,"\x7f""ELF",4) && header[4]==2 && header[18]==183);
        unsigned char *all=malloc(file.size);T(all!=NULL);T(lume_fat_read(fat,file,0,all,file.size)==0);
        for(unsigned offset=0;offset<file.size;offset+=499){unsigned char b[777];unsigned n=file.size-offset;if(n>sizeof(b))n=sizeof(b);T(lume_fat_read(fat,file,offset,b,n)==0 && !memcmp(b,all+offset,n));}
        T(lume_fat_read(fat,file,file.size,header,1)==-1);free(all);
    }
    return true;
}
int main(int argc,char **argv)
{
    T(argc==2);FILE *in=fopen(argv[1],"rb");T(in!=NULL);T(!fseek(in,0,SEEK_END));long bytes=ftell(in);rewind(in);
    unsigned char *data=malloc((size_t)bytes),*bad=malloc((size_t)bytes);T(data && bad);T(fread(data,1,(size_t)bytes,in)==(size_t)bytes);fclose(in);
    struct lume_fat fat;T(lume_fat_mount(&fat,data,(size_t)bytes));T(lume_fat_walk(&fat,visit,&fat));T(dirs==4 && files==24);
    for(unsigned n=0;n<512;++n)T(!lume_fat_mount(&fat,data,n));
    unsigned offsets[]={11,13,14,16,17,22,32,36,42,44,510};
    unsigned sizes[]={2,1,2,1,2,2,4,4,2,4,1};
    unsigned values[]={1024,3,0,0,1,1,0,0,1,1,0};
    for(unsigned n=0;n<sizeof(offsets)/sizeof(offsets[0]);++n){memcpy(bad,data,(size_t)bytes);for(unsigned k=0;k<sizes[n];++k)bad[offsets[n]+k]=(unsigned char)(values[n]>>(k*8));T(!lume_fat_mount(&fat,bad,(size_t)bytes));}
    T(lume_fat_mount(&fat,bad,(size_t)bytes-1)==false);
    memcpy(bad,data,(size_t)bytes);T(lume_fat_mount(&fat,bad,(size_t)bytes));
    memset(bad+fat.first,0xe5,fat.cluster_bytes);unsigned off=fat.fat+fat.root*4;bad[off]=2;bad[off+1]=bad[off+2]=bad[off+3]=0;
    T(!lume_fat_walk(&fat,visit,&fat));
    printf("PASS_FAT32_VOLUME checks=%u files=24 dirs=4\n",checks);free(data);free(bad);
}
