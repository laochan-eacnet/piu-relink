#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void conversion(const char *name)
{
    const char *selected=setlocale(LC_CTYPE,name);
    if(!selected) {printf("unavailable %s\n",name);return;}
    printf("locale %s max %u\n",name,(unsigned)MB_CUR_MAX);
    const char *samples[]={"A","\x7f","\x80","\xc3\xa9","\xea\xb0\x80","\xf0\x9f\x98\x80","\xed\xa0\x80","\xf8\x88\x80\x80\x80",""};
    for(unsigned i=0;i<sizeof(samples)/sizeof(*samples);++i) {
        mbstate_t st={0};wchar_t wc=0x1234;errno=0;size_t n=mbrtowc(&wc,samples[i],strlen(samples[i])+1,&st);
        printf("decode %u %d %x %d %d\n",i,(int)n,(unsigned)wc,errno,mbsinit(&st));
    }
    wchar_t values[]={0,'A',0x7f,0x80,0xe9,0xac00,0x1f600,0xd800,0x200000};
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i) {
        char buf[16]={0};mbstate_t st={0};errno=0;size_t n=wcrtomb(buf,values[i],&st);
        printf("encode %x %d %d",(unsigned)values[i],(int)n,errno);
        if(n!=(size_t)-1) for(unsigned j=0;j<n;++j) printf(" %02x",(unsigned char)buf[j]);puts("");
    }
    mbstate_t st={0};wchar_t wc=0;errno=0;size_t a=mbrtowc(&wc,"\xea",1,&st);int init=mbsinit(&st);size_t b=mbrtowc(&wc,"\xb0\x80",2,&st);
    printf("split %d %d %d %x %d\n",(int)a,init,(int)b,(unsigned)wc,errno);
    const char *p="A\xea\xb0\x80Z";wchar_t ws[8]={0};memset(&st,0,sizeof(st));errno=0;
    size_t n=mbsnrtowcs(ws,&p,3,8,&st);printf("nrtowcs %d %x %d %d\n",(int)n,(unsigned)ws[0],p?*p:-1,mbsinit(&st));
    if(n!=(size_t)-1) {n=mbsrtowcs(ws,&p,8,&st);printf("rtowcs %d %x %x %d\n",(int)n,(unsigned)ws[0],(unsigned)ws[1],p==NULL);}
    const wchar_t *wp=L"A\uac00Z";char buf[16]={0};memset(&st,0,sizeof(st));errno=0;
    n=wcsnrtombs(buf,&wp,3,3,&st);printf("nrtombs %d %x %d\n",(int)n,(unsigned)*wp,errno);
    printf("width %d %d %d %d %d\n",wcwidth(L'A'),wcwidth(0x301),wcwidth(0xac00),wcwidth(0x1f600),wcwidth(L'\n'));
}
int main(void)
{
    conversion("C");conversion("C.UTF-8");
    wchar_t buf[32];wcscpy(buf,L"alpha");wcscat(buf,L"beta");
    printf("strings %u %d %d %u %u\n",(unsigned)wcslen(buf),wcscmp(L"a",L"b"),wcsncmp(L"abc",L"abd",2),(unsigned)(wcsstr(buf,L"beta")-buf),(unsigned)(wcsrchr(buf,L'a')-buf));
    wchar_t *end;errno=0;long v=wcstol(L"-0x2a tail",&end,0);printf("wcstol %ld %x %d\n",v,(unsigned)*end,errno);
    errno=0;v=wcstol(L"-99999999999999999999",&end,10);printf("overflow %ld %d\n",v,errno);
    double d=wcstod(L" 1.25rest",&end);printf("wcstod %.17g %x\n",d,(unsigned)*end);
    return 0;
}
