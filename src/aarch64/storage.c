/* The block transport exposes only the identified third partition. FatFs
 * never sees the partition table, boot FAT or Pi OS root. No formatting API. */
#include "lume/early.h"
#include "lume/sd.h"
#include "lume/storage.h"
#include "lume/abi.h"
#include "ff.h"
#include "diskio.h"
static struct lume_volume volume;
static FATFS filesystem;
static bool mounted;
DSTATUS disk_initialize(BYTE drive) {return drive || !volume.count?STA_NOINIT:0;}
DSTATUS disk_status(BYTE drive) {return disk_initialize(drive);}
DRESULT disk_read(BYTE drive,BYTE *out,LBA_t sector,UINT count) {
    if(drive || !count || sector>=volume.count || count>volume.count-sector)return RES_PARERR;
    for(UINT i=0;i<count;++i)if(!lume_sd_read(volume.first+sector+i,out+(size_t)i*512))return RES_ERROR;
    return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *data,LBA_t sector,UINT count) {
    if(drive || !mounted || !count || sector>=volume.count || count>volume.count-sector)return RES_PARERR;
    for(UINT i=0;i<count;++i)if(!lume_sd_write(volume.first+sector+i,data+(size_t)i*512))return RES_ERROR;
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *data) {
    if(drive || !volume.count)return RES_PARERR;
    switch(command) {
    case CTRL_SYNC:return RES_OK; /* Every CMD24 waits until card ready. */
    case GET_SECTOR_COUNT:*(LBA_t *)data=(LBA_t)volume.count;return RES_OK;
    case GET_SECTOR_SIZE:*(WORD *)data=512;return RES_OK;
    case GET_BLOCK_SIZE:*(DWORD *)data=1;return RES_OK;
    default:return RES_PARERR;
    }
}
static int error(FRESULT r) {
    switch(r) {
    case FR_OK:return 0;
    case FR_NO_FILE:case FR_NO_PATH:return -LUME_ENOENT;
    case FR_INVALID_NAME:case FR_INVALID_PARAMETER:return -LUME_EINVAL;
    case FR_DENIED:return -LUME_EACCES;
    case FR_EXIST:return -LUME_EEXIST;
    case FR_WRITE_PROTECTED:return -LUME_EROFS;
    case FR_NOT_ENOUGH_CORE:return -LUME_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:return -LUME_EMFILE;
    default:return -LUME_EIO;
    }
}
static const char *name(const char *path) {
    if(path[0]!='/' || path[1]!='d' || path[2]!='a' || path[3]!='t' || path[4]!='a' ||
       (path[5] && path[5]!='/'))return 0;
    return path[5]?path+5:"/";
}
static int create(const char *path,bool directory) {
    const char *p=name(path);if(!p)return -LUME_EINVAL;
    if(directory)return error(f_mkdir(p));
    FIL file;FRESULT r=f_open(&file,p,FA_WRITE|FA_CREATE_NEW);
    return error(r==FR_OK?f_close(&file):r);
}
static int truncate_file(const char *path) {
    const char *p=name(path);if(!p)return -LUME_EINVAL;
    FIL file;FRESULT r=f_open(&file,p,FA_WRITE|FA_OPEN_EXISTING);
    if(r!=FR_OK)return error(r);
    r=f_truncate(&file);FRESULT end=f_close(&file);return error(r==FR_OK?end:r);
}
static int read_file(const char *path,uint64_t offset,void *data,size_t bytes) {
    const char *p=name(path);if(!p || offset>UINT32_MAX || bytes>INT32_MAX)return -LUME_EINVAL;
    FIL file;FRESULT r=f_open(&file,p,FA_READ);if(r!=FR_OK)return error(r);
    UINT n=0;r=f_lseek(&file,(FSIZE_t)offset);
    if(r==FR_OK)r=f_read(&file,data,(UINT)bytes,&n);
    FRESULT end=f_close(&file);if(r==FR_OK)r=end;
    return r==FR_OK?(int)n:error(r);
}
static int write_file(const char *path,uint64_t offset,const void *data,size_t bytes) {
    const char *p=name(path);if(!p || offset>UINT32_MAX || bytes>UINT32_MAX-offset || bytes>INT32_MAX)return -LUME_EINVAL;
    FIL file;FRESULT r=f_open(&file,p,FA_WRITE);if(r!=FR_OK)return error(r);
    UINT n=0;
    if(offset>f_size(&file)) {
        static const unsigned char zero[512]={0};
        r=f_lseek(&file,f_size(&file));
        while(r==FR_OK && f_tell(&file)<offset) {
            UINT gap=offset-f_tell(&file)>sizeof(zero)?sizeof(zero):(UINT)(offset-f_tell(&file)),done=0;
            r=f_write(&file,zero,gap,&done);
            if(r==FR_OK && done!=gap)r=FR_DENIED;
        }
    } else r=f_lseek(&file,(FSIZE_t)offset);
    if(r==FR_OK && f_tell(&file)!=(FSIZE_t)offset)r=FR_DENIED;
    if(r==FR_OK)r=f_write(&file,data,(UINT)bytes,&n);
    if(r==FR_OK)r=f_sync(&file);
    FRESULT end=f_close(&file);if(r==FR_OK)r=end;
    return r==FR_OK?(int)n:error(r);
}
static int remove_file(const char *path) {
    const char *p=name(path);return p?error(f_unlink(p)):-LUME_EINVAL;
}
static bool walk_directory(char path[128],unsigned length,unsigned depth,lume_storage_visit visit,void *context) {
    if(depth>8)return false;
    DIR dir;FRESULT r=f_opendir(&dir,name(path));if(r!=FR_OK)return false;
    FILINFO entry;bool ok=true;
    for(;;) {
        r=f_readdir(&dir,&entry);if(r!=FR_OK){ok=false;break;}
        if(!entry.fname[0])break;
        unsigned n=length;if(n>=126){ok=false;break;}path[n++]='/';
        for(unsigned i=0;entry.fname[i];++i) {
            unsigned char c=(unsigned char)entry.fname[i];
            if(n>=126 || c<33 || c>=127){ok=false;break;}
            path[n++]=(char)(c>='A' && c<='Z'?c+32:c);
        }
        path[n]=0;bool directory=(entry.fattrib&AM_DIR)!=0;
        if(!ok || !visit(path,directory,entry.fsize,context) ||
           (directory && !walk_directory(path,n,depth+1,visit,context))){ok=false;break;}
        path[length]=0;
    }
    return f_closedir(&dir)==FR_OK && ok;
}
static bool walk(lume_storage_visit visit,void *context) {
    char path[128]="/data";return walk_directory(path,5,0,visit,context);
}
const struct lume_storage_ops *lume_storage_start(void) {
    mounted=false;volume=(struct lume_volume){0};unsigned char block[512];
    if(!lume_sd_read(0,block) || !lume_volume_select(block,lume_sd_sectors(),&volume))return 0;
    if(!lume_sd_read(volume.first,block))return 0;
    /* Bind writes to the freshly provisioned FAT32 volume, including ID/label. */
    static const unsigned char label[11]="LUME_DATA  ";
    if(block[510]!=0x55 || block[511]!=0xaa || block[11]!=0 || block[12]!=2 ||
       block[67]!=0x45 || block[68]!=0x4d || block[69]!=0x55 || block[70]!=0x4c)return 0;
    for(unsigned i=0;i<11;++i)if(block[71+i]!=label[i])return 0;
    if(f_mount(&filesystem,"",1)!=FR_OK || filesystem.fs_type!=FS_FAT32 ||
       filesystem.volbase!=0 || filesystem.database>=volume.count)return 0;
    if(!lume_sd_allow_volume(volume.first,volume.count))return 0;
    mounted=true;lume_value("SD_LUME_FIRST_SECTOR",volume.first);lume_value("SD_LUME_SECTORS",volume.count);
    static const struct lume_storage_ops ops={walk,create,truncate_file,read_file,write_file,remove_file};
    return &ops;
}
