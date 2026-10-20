#include "fat_ro.h"
#define END 0x0ffffff8u
#define HOPS 8192u
#define BULK_SECTORS 32u
static uint32_t le16(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t le32(const unsigned char *p){return le16(p)|(le16(p+2)<<16);}
static int fetch(struct fat_ro *f,uint32_t lba){
    if(lba>=f->blocks)return 0;
    if(f->cached==lba)return 1;
    f->cached=UINT32_MAX;
    if(!f->read(f->context,lba,f->sector))return 0;
    f->cached=lba;return 1;
}
static int valid_cluster(const struct fat_ro *f,uint32_t c){return c>=2&&c-2<f->clusters;}
static uint32_t next(struct fat_ro *f,uint32_t c){
    if(!valid_cluster(f,c)||!fetch(f,f->fat+c/128u))return 0;
    uint32_t n=le32(f->sector+(c%128u)*4u)&0x0fffffffu;
    if(n==c)return 0;
    return n>=END?END:valid_cluster(f,n)?n:0;
}
static int bpb(struct fat_ro *f,uint32_t start,uint32_t partition_bytes){
    if(!fetch(f,start)||f->sector[510]!=0x55||f->sector[511]!=0xaa)return 0;
    const unsigned char *s=f->sector;
    uint32_t reserved=le16(s+14),fats=s[16],fat_size=le32(s+36),total=le32(s+32);
    uint32_t spc=s[13],flags=le16(s+40),root=le32(s+44);
    if(le16(s+11)!=512||!spc||spc>128||(spc&(spc-1))||!reserved||fats<1||fats>2||
       le16(s+17)||le16(s+19)||le16(s+22)||le16(s+42)||!fat_size||
       start>=f->blocks||total>f->blocks-start||total>partition_bytes||total<=reserved||
       fat_size>(total-reserved)/fats)return 0;
    uint32_t overhead=reserved+fats*fat_size,count=(total-overhead)/spc;
    if(count<65525||count>0x0fffffedu||fat_size<(count+129u)/128u)return 0;
    uint32_t active=(flags&128u)?flags&15u:0;
    if(active>=fats||root<2||root-2>=count)return 0;
    f->start=start;f->total=total;f->fat=start+reserved+active*fat_size;
    f->data=start+overhead;f->clusters=count;f->root=root;f->sectors_per_cluster=spc;return 1;
}
int fat_ro_mount(struct fat_ro *f,fat_ro_sector_read reader,void *ctx,uint32_t blocks){
    f->read=reader;f->read_blocks=0;f->context=ctx;f->blocks=blocks;f->cached=UINT32_MAX;
    if(!blocks||!fetch(f,0)||f->sector[510]!=0x55||f->sector[511]!=0xaa)return 0;
    if(le16(f->sector+11)==512&&bpb(f,0,blocks))return 1;
    if(!fetch(f,0))return 0;
    uint32_t starts[4],sizes[4];unsigned types[4];
    for(unsigned i=0;i<4;i++){
        const unsigned char *p=f->sector+446+i*16;
        starts[i]=le32(p+8);sizes[i]=le32(p+12);types[i]=p[4];
    }
    for(unsigned i=0;i<4;i++)if((types[i]==0x0b||types[i]==0x0c)&&starts[i]&&sizes[i]&&
        starts[i]<blocks&&sizes[i]<=blocks-starts[i]&&bpb(f,starts[i],sizes[i]))return 1;
    return 0;
}
void fat_ro_set_block_reader(struct fat_ro *f,fat_ro_block_read reader){f->read_blocks=reader;}
static int sector_for(struct fat_ro *f,uint32_t first,uint32_t sector,uint32_t *result){
    uint32_t c=first,hops=sector/f->sectors_per_cluster;
    if(hops>HOPS)return 0;
    while(hops--){c=next(f,c);if(c==END)return -1;if(!c)return 0;}
    if(!valid_cluster(f,c))return 0;
    *result=f->data+(c-2u)*f->sectors_per_cluster+sector%f->sectors_per_cluster;
    return *result-f->start<f->total;
}
int fat_ro_list(struct fat_ro *f,const struct fat_ro_file *dir,uint32_t *cursor,struct fat_ro_entry *e){
    if(!dir->directory)return -1;
    while(*cursor<4096u){
        uint32_t n=(*cursor)++,lba;
        int status=sector_for(f,dir->first,n/16u,&lba);
        if(status<0)return 0;if(!status)return -1;
        if(!fetch(f,lba))return -1;
        const unsigned char *p=f->sector+(n%16u)*32u;
        if(!p[0])return 0;
        if(p[0]==0xe5||p[11]==0x0f||(p[11]&8u))continue;
        unsigned used=0;
        for(unsigned i=0;i<8&&p[i]!=' ';i++)e->name[used++]=(char)p[i];
        if(p[8]!=' '){e->name[used++]='.';for(unsigned i=8;i<11&&p[i]!=' ';i++)e->name[used++]=(char)p[i];}
        e->name[used]=0;e->cluster=((le16(p+20)<<16)|le16(p+26))&0x0fffffffu;
        e->size=le32(p+28);e->directory=!!(p[11]&16u);return 1;
    }
    return -1;
}
static unsigned upper(unsigned c){return c>='a'&&c<='z'?c-'a'+'A':c;}
static int equal(const char *a,const char *b){
    while(*a&&*b){if(upper((unsigned char)*a++)!=upper((unsigned char)*b++))return 0;}
    return !*a&&!*b;
}
int fat_ro_open(struct fat_ro *f,const char *path,struct fat_ro_file *file){
    file->first=f->root;file->size=0;file->directory=1;
    while(*path){
        while(*path=='/')path++;if(!*path)break;
        char part[13];unsigned used=0;
        while(*path&&*path!='/'){if(used==12)return 0;part[used++]=*path++;}part[used]=0;
        if(equal(part,"."))continue;
        if(file->first==f->root&&equal(part,".."))continue;
        if(!file->directory)return 0;
        uint32_t cursor=0;struct fat_ro_entry e;int found=0,status;
        while((status=fat_ro_list(f,file,&cursor,&e))>0)if(equal(part,e.name)){found=1;break;}
        if(!found)return 0;
        file->first=e.cluster?e.cluster:(e.directory?f->root:0);file->size=e.size;file->directory=e.directory;
        if((file->directory||file->size)&&!valid_cluster(f,file->first))return 0;
    }
    file->cluster=file->first;file->index=0;file->position=0;return 1;
}
int fat_ro_seek(struct fat_ro_file *file,uint32_t position){
    if(file->directory||position>file->size)return 0;
    file->position=position;file->cluster=file->first;file->index=0;return 1;
}
int fat_ro_read(struct fat_ro *f,struct fat_ro_file *file,unsigned char *out,unsigned count){
    if(file->directory||file->position>file->size)return -1;
    if(count>file->size-file->position)count=file->size-file->position;
    unsigned done=0;uint32_t cluster_bytes=f->sectors_per_cluster*512u;
    while(done<count){
        uint32_t wanted=file->position/cluster_bytes;
        if(wanted>HOPS)return -1;
        while(file->index<wanted){file->cluster=next(f,file->cluster);file->index++;}
        if(!valid_cluster(f,file->cluster))return -1;
        uint32_t lba=f->data+(file->cluster-2u)*f->sectors_per_cluster+(file->position%cluster_bytes)/512u;
        unsigned offset=file->position%512u;
        if(!offset&&f->read_blocks&&count-done>=1024u){
            /* Walk only the requested extent and merge proven physical
             * neighbours, including SPC=1 files. Never assume allocation is
             * contiguous or follow more clusters than the existing hop bound. */
            unsigned limit=(count-done)/512u;if(limit>BULK_SECTORS)limit=BULK_SECTORS;
            unsigned run=f->sectors_per_cluster-(file->position%cluster_bytes)/512u;
            if(run>limit)run=limit;
            uint32_t last=file->cluster,index=file->index;
            while(run<limit&&index<HOPS){
                uint32_t following=next(f,last);
                if(!valid_cluster(f,following)||following!=last+1u)break;
                unsigned extra=f->sectors_per_cluster;if(extra>limit-run)extra=limit-run;
                run+=extra;last=following;index++;
            }
            if(run>1){
                if(lba<f->start||lba-f->start>=f->total||run>f->total-(lba-f->start)||
                   lba>=f->blocks||run>f->blocks-lba||!f->read_blocks(f->context,lba,run,out+done))return -1;
                unsigned take=run*512u;file->position+=take;done+=take;
                file->cluster=last;file->index=index;continue;
            }
        }
        if(!fetch(f,lba))return -1;
        unsigned take=512u-offset;if(take>count-done)take=count-done;
        for(unsigned i=0;i<take;i++)out[done+i]=f->sector[offset+i];
        file->position+=take;done+=take;
    }
    return (int)done;
}
