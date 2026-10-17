#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
__asm__(".text\n.p2align 12\n.globl image_code\n.type image_code,@function\nimage_code:\n.byte 0xb8,11,0,0,0,0xc3\n.size image_code,.-image_code\n");
extern int image_code(void);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
int main(void)
{
    const char *path="/tmp/lxrt-fs-mapping";int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);CHECK(ftruncate(fd,131072)==0);CHECK(pwrite(fd,"mapped",6,4096)==6);
    char *p=mmap(NULL,8192,PROT_READ,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED);printf("file %.*s\n",6,p+4096);CHECK(munmap(p,8192)==0);
    p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,4096);CHECK(p!=MAP_FAILED);CHECK(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);p[0]='C';CHECK(mprotect(p,4096,PROT_READ)==0);printf("private upgraded %c\n",p[0]);CHECK(munmap(p,4096)==0);
    p=mmap64(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,4096);CHECK(p!=MAP_FAILED);printf("offset %.*s\n",6,p);p[0]='M';CHECK(msync(p,4096,MS_SYNC)==0);CHECK(munmap(p,4096)==0);char b[7]={0};CHECK(pread(fd,b,6,4096)==6);printf("private %s\n",b);
    p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,4096);CHECK(p!=MAP_FAILED);memcpy(p,"shared",6);CHECK(msync(p,4096,MS_SYNC)==0);CHECK(munmap(p,4096)==0);CHECK(pread(fd,b,6,4096)==6);printf("shared %s\n",b);close(fd);unlink(path);
    fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);CHECK(write(fd,"small",5)==5);p=mmap(NULL,5,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);printf("small %.*s\n",5,p);CHECK(munmap(p,5)==0);close(fd);unlink(path);
    p=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);printf("anonymous zero=%d\n",p[0]==0&&p[12287]==0);strcpy(p,"anonymous");CHECK(mprotect(p,4096,PROT_READ)==0);printf("protect %s\n",p);CHECK(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);CHECK(munmap(p+4096,4096)==0);CHECK(munmap(p,4096)==0);CHECK(munmap(p+8192,4096)==0);
    p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);strcpy(p,"remap");char *q=mremap(p,4096,16384,MREMAP_MAYMOVE);CHECK(q!=MAP_FAILED);printf("remap %s zero=%d\n",q,q[16383]==0);CHECK(madvise(q,4096,MADV_DONTNEED)==0);printf("discard zero=%d\n",q[0]==0);CHECK(munmap(q,16384)==0);
    unsigned char *code=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(code!=MAP_FAILED);unsigned char bytes[]={0xb8,42,0,0,0,0xc3};memcpy(code,bytes,sizeof bytes);CHECK(mprotect(code,4096,PROT_READ|PROT_EXEC)==0);int (*fn)(void)=(void *)code;printf("execute %d\n",fn());CHECK(mprotect(code,4096,PROT_READ|PROT_WRITE|PROT_EXEC)==0);code[1]=7;CHECK(mprotect(code,4096,PROT_READ|PROT_EXEC)==0);printf("patched %d\n",fn());CHECK(munmap(code,4096)==0);
    p=mmap(NULL,131072,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);uintptr_t fixed=(((uintptr_t)p+65535)&~(uintptr_t)65535)+4096;CHECK(munmap(p,131072)==0);q=mmap((void *)fixed,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);CHECK(q==(void *)fixed);printf("fixed page=%d\n",q==(void *)fixed);CHECK(munmap(q,4096)==0);
    printf("image before=%d\n",image_code());unsigned char *image=(void *)image_code;void *page=(void *)((uintptr_t)image&~(uintptr_t)4095);CHECK(mprotect(page,4096,PROT_READ|PROT_WRITE|PROT_EXEC)==0);image[1]=19;CHECK(mprotect(page,4096,PROT_READ|PROT_EXEC)==0);printf("image patched=%d\n",image_code());
    errno=0;CHECK(mmap(NULL,0,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED&&errno==EINVAL);puts("mappings ok");return 0;
}
