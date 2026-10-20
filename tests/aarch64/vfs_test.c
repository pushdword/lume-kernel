#include "lume/vfs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define T(x) do{assert(x);++checks;}while(0)
int lume_console_getc(void){return -LUME_EAGAIN;}
void lume_console_putc(char c){(void)c;}
int main(void)
{
    struct lume_files f,g;const char elf[]="ELF";
    lume_vfs_init(elf,3);lume_files_init(&f);lume_files_init(&g);
    const void *p;size_t bytes;
    T(lume_vfs_image("/bin/sh",&p,&bytes) && bytes==3 && p==elf);
    T(!lume_vfs_image("/tmp",&p,&bytes));
    T(lume_vfs_open(&f,"",0)==-LUME_ENOENT);
    T(lume_vfs_open(&f,"/etc/motd/../motd",0)==-LUME_ENOTDIR);
    T(lume_vfs_open(&f,"/missing/../etc/motd",0)==-LUME_ENOENT);
    T(lume_vfs_open(&f,"/tmp/missing/file",LUME_O_CREAT|LUME_O_RDWR)==-LUME_ENOENT);
    T(lume_vfs_open(&f,"/tmp/file/",LUME_O_CREAT|LUME_O_RDWR)==-LUME_ENOENT);
    T(lume_vfs_open(&f,"/etc/motd",LUME_O_WRONLY)==-LUME_EROFS);
    T(lume_vfs_open(&f,"/etc/new",LUME_O_CREAT|LUME_O_RDWR)==-LUME_EROFS);
    T(lume_vfs_open(&f,"/tmp",LUME_O_RDWR)==-LUME_EISDIR);
    T(lume_vfs_open(&f,"/tmp",LUME_O_TRUNC)==-LUME_EINVAL);
    T(lume_vfs_open(&f,"/tmp",3)==-LUME_EINVAL);
    T(lume_vfs_open(&f,"/tmp",UINT64_MAX)==-LUME_EINVAL);
    T(lume_vfs_chdir(&f,"/tmp/./../tmp/")==0 && !strcmp(f.cwd,"/tmp"));
    T(!strcmp(g.cwd,"/"));
    T(lume_vfs_chdir(&f,"/etc/motd")==-LUME_ENOTDIR);
    int fd=(int)lume_vfs_open(&f,"note",LUME_O_CREAT|LUME_O_RDWR);
    T(fd==3);T(lume_vfs_write(&f,fd,"abc",3)==3);
    int other=(int)lume_vfs_open(&g,"/tmp/note",0);T(other==3);
    char b[4096]={0};T(lume_vfs_read(&g,other,b,3)==3 && !memcmp(b,"abc",3));
    T(lume_vfs_seek(&f,fd,10,LUME_SEEK_SET)==10);
    T(lume_vfs_write(&f,fd,"",0)==0);
    struct lume_stat st;T(lume_vfs_stat(&f,fd,&st)==0 && st.size==3);
    T(lume_vfs_write(&f,fd,"z",1)==1);
    T(lume_vfs_seek(&f,fd,0,0)==0);
    T(lume_vfs_read(&f,fd,b,12)==11 && b[10]=='z');
    for(unsigned i=3;i<10;++i)T(b[i]==0);
    T(lume_vfs_seek(&f,fd,INT64_MIN,1)==-LUME_EINVAL);
    T(lume_vfs_seek(&f,fd,INT64_MAX,0)==INT64_MAX);
    T(lume_vfs_seek(&f,fd,1,1)==-LUME_EINVAL);
    T(lume_vfs_read(&f,fd,b,1)==0);
    T(lume_vfs_write(&f,fd,b,1)==-LUME_ENOSPC);
    T(lume_vfs_seek(&f,fd,0,3)==-LUME_EINVAL);
    T(lume_vfs_close(&f,fd)==0 && lume_vfs_close(&f,fd)==-LUME_EBADF);
    fd=(int)lume_vfs_open(&f,"note",LUME_O_WRONLY|LUME_O_APPEND);T(fd==3);
    T(lume_vfs_write(&f,fd,"a",1)==1);
    T(lume_vfs_read(&f,fd,b,1)==-LUME_EBADF);T(lume_vfs_close(&f,fd)==0);
    T(lume_vfs_read(&g,other,b,20)==9 && b[8]=='a');
    T(lume_vfs_write(&g,other,"x",1)==-LUME_EBADF);
    T(lume_vfs_close(&g,other)==0);
    fd=(int)lume_vfs_open(&f,"/",0);T(fd==3);
    struct lume_dirent ent;unsigned dirs=0;
    T(lume_vfs_read(&f,fd,b,1)==-LUME_EISDIR);
    T(lume_vfs_getdents(&f,fd,b,63)==-LUME_EINVAL);
    while(lume_vfs_getdents(&f,fd,&ent,sizeof(ent))>0){T(ent.kind==LUME_NODE_DIR && !ent.reserved);++dirs;}
    T(dirs==4);T(lume_vfs_close(&f,fd)==0);
    T(lume_vfs_read(&f,0,b,1)==-LUME_EAGAIN);
    T(lume_vfs_seek(&f,0,0,0)==-LUME_ESPIPE);
    for(unsigned i=3;i<LUME_FD_MAX;++i)T(lume_vfs_open(&f,"/etc/motd",0)==i);
    T(lume_vfs_open(&f,"/etc/motd",0)==-LUME_EMFILE);
    for(unsigned i=3;i<LUME_FD_MAX;++i)T(lume_vfs_close(&f,i)==0);
    char longpath[200];memset(longpath,'a',sizeof(longpath));longpath[199]=0;
    T(lume_vfs_open(&f,longpath,0)==-LUME_ENAMETOOLONG);
    T(lume_vfs_mkdir(&f,"/tmp/a")==0);
    T(lume_vfs_mkdir(&f,"/tmp/a")==-LUME_EEXIST);
    T(lume_vfs_mkdir(&f,"/etc/a")==-LUME_EROFS);
    T(lume_vfs_mkdir(&f,"/tmp/a/b")==0);
    fd=(int)lume_vfs_open(&f,"/tmp/a/b/file",LUME_O_CREAT|LUME_O_RDWR);T(fd>=3);
    T(lume_vfs_write(&f,fd,"data",4)==4);
    T(lume_vfs_dup2(&f,fd,15)==15);
    T(lume_vfs_dup2(&f,99,15)==-LUME_EBADF);
    T(lume_vfs_remove(&f,"/tmp/a/b",true)==-LUME_ENOTEMPTY);
    T(lume_vfs_remove(&f,"/tmp/a/b/file",true)==-LUME_ENOTDIR);
    T(lume_vfs_remove(&f,"/tmp/a/b/file",false)==0);
    T(lume_vfs_open(&f,"/tmp/a/b/file",0)==-LUME_ENOENT);
    T(lume_vfs_seek(&f,15,0,0)==0 && lume_vfs_read(&f,15,b,4)==4 && !memcmp(b,"data",4));
    T(lume_vfs_close(&f,15)==0 && lume_vfs_close(&f,fd)==0);
    T(lume_vfs_remove(&f,"/tmp/a/b",true)==0 && lume_vfs_remove(&f,"/tmp/a",true)==0);
    T(lume_vfs_remove(&f,"/tmp",true)==-LUME_EROFS);
    /* Exhaust finite RAM nodes, independently of open descriptor limits. */
    int result=0;
    for(unsigned i=0;i<200;++i){char name[40];snprintf(name,sizeof(name),"/tmp/n%u",i);
        result=(int)lume_vfs_open(&f,name,LUME_O_CREAT|LUME_O_RDWR);
        if(result<0)break;
        T(lume_vfs_close(&f,result)==0);
    }
    T(result==-LUME_ENOSPC);
    printf("VFS RAM paths/fds/permissions/seek/directories/limits: %u checks PASS\n",checks);
    return 0;
}
