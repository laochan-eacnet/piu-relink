#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <unistd.h>

/* Each row is a deterministic return value plus bytes, not an assertion against
 * a second formatter. Run the same executable under qemu/glibc and lxrt and
 * byte-diff stdout. Hex keeps embedded NULs and wide/truncation cases visible.
 * Windows ARM64's x86 emulator rounds both x87 registers on `fld st(0)`,
 * although plain fldt/fstpt preserves all 80 bits. Optimized guest argument
 * duplication can therefore lose precision before entering the HLE. Exact
 * long-double tables memcpy the original 12-byte arguments into guest va_list;
 * ordinary varargs still cover exact basic values. The separate x87-transfer
 * mode reproduces the emulator defect rather than hiding it in comparisons. */
static unsigned row;
static void bytes(const unsigned char *p, size_t n)
{
    static const char hex[]="0123456789abcdef";
    for (size_t i=0; i<n; ++i) {
        putchar(hex[p[i]>>4]); putchar(hex[p[i]&15]);
    }
}
static void report(const char *label, int result, const char *s, size_t n)
{
    printf("%03u %s %d ",row++,label,result);
    bytes((const unsigned char *)s,n);
    putchar('\n');
}
static void check_list(const char *fmt, va_list ap)
{
    char out[24000];
    int n=vsnprintf(out,sizeof out,fmt,ap);
    report(fmt,n,out,n<0 ? strlen(out) : (size_t)n<sizeof out ? (size_t)n : sizeof out-1);
}
static void check(const char *fmt, ...)
{
    va_list ap;
    va_start(ap,fmt);
    check_list(fmt,ap);
    va_end(ap);
}
static void check_long(const char *fmt, const long double *value)
{
    unsigned char raw[12];
    memcpy(raw,value,sizeof raw);
    check_list(fmt,(char *)raw);
}
static void check_wide(const wchar_t *fmt, ...)
{
    wchar_t out[256];
    for (size_t i=0; i<sizeof out/sizeof *out; ++i) out[i]=0x7777;
    va_list ap;
    va_start(ap,fmt);
    int n=vswprintf(out,sizeof out/sizeof *out,fmt,ap);
    va_end(ap);
    printf("%03u wide %d",row++,n);
    size_t length=n<0 ? 0 : (size_t)n;
    for (size_t i=0; i<length; ++i) printf(" %08x",(unsigned)out[i]);
    putchar('\n');
}
static void integers(void)
{
    static const char *const flags[]={"","0","-","+","#","'"};
    static const char conversions[]="diuoxX";
    static const uint32_t values[]={0,1,0x80000000u,0xffffffffu};
    char fmt[48];
    for (size_t c=0; c<sizeof conversions-1; ++c)
        for (size_t f=0; f<sizeof flags/sizeof *flags; ++f)
            for (size_t v=0; v<sizeof values/sizeof *values; ++v) {
                sprintf(fmt,"%%%s12%c",flags[f],conversions[c]);
                if (c<2) check(fmt,(int32_t)values[v]);
                else check(fmt,values[v]);
            }
    static const char *const specs[]={"%.0d","%#.0o","%#.0x","%#.1o","%#08x","%#08.4x","%+08.5d","%-+12.4d","% 010d","%'020u","%*.*d","%0*.*d"};
    for (size_t i=0; i<sizeof specs/sizeof *specs; ++i) {
        if (i>=10) {
            check(specs[i],12,5,-17); check(specs[i],-12,-1,17);
        } else {
            check(specs[i],0); check(specs[i],17); check(specs[i],-17);
        }
    }
    check("%hhd %hhu %hd %hu",0x180,0x1ff,0x18000,0x1ffff);
    check("%ld %lu %zd %zu %td %tu",(-2147483647L-1),4294967295UL,(ssize_t)-1,(size_t)-1,(ptrdiff_t)-2,(size_t)-2);
    check("%lld %llu %llx %llo",INT64_MIN,UINT64_MAX,UINT64_MAX,UINT64_MAX);
    check("%jd %ju %jx",(intmax_t)INT64_MIN,(uintmax_t)UINT64_MAX,(uintmax_t)UINT64_MAX);
    check("%qd %qu %qx",INT64_MIN,UINT64_MAX,UINT64_MAX);
    check("%2$d/%1$d/%2$08x",11,-22);
    check("%2$lld/%1$d/%3$llu",7,INT64_MIN,UINT64_MAX);
    check("%3$*1$.*2$d",14,7,-19);
    check("%3$*1$.*2$d",-14,-2,-19);
    check("%1$d %2$d %3$d %4$d %5$d %6$d %7$d %8$d %9$d %10$d %11$d %12$d %13$d %14$d %15$d %16$d %17$d %18$d %19$d %20$d %21$d %22$d %23$d %24$d %25$d %26$d %27$d %28$d %29$d %30$d %31$d %32$d %33$d %34$d %35$d",1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35);
}
static void floating(void)
{
    static const double values[]={0.0,-0.0,1.0,-1.5,0.1,1.0/3.0,0x1.fffffffffffffp-1,0x1p-1022,0x0.0000000000001p-1022,1e-7,1e20,INFINITY,-INFINITY,NAN,-NAN};
    static const char *const specs[]={"%+16.6e","% 016.4E","%#18.8f","%-18.3F","%.6g","%#.9G"};
    for (size_t f=0; f<sizeof specs/sizeof *specs; ++f)
        for (size_t v=0; v<sizeof values/sizeof *values; ++v) check(specs[f],values[v]);
    static const char *const hexspecs[]={"%a","%A","%#.0a","%+022.3a","%-24.13A"};
    for (size_t f=0; f<sizeof hexspecs/sizeof *hexspecs; ++f)
        for (size_t v=0; v<sizeof values/sizeof *values; ++v) check(hexspecs[f],values[v]);
    static const double ties[]={0.5,1.5,2.5,3.5,1.25,1.75,9.999999,99.5};
    for (size_t i=0; i<sizeof ties/sizeof *ties; ++i) {
        check("%.0f/%.1f/%.2g/%.0a/%.1a",ties[i],ties[i],ties[i],ties[i],ties[i]);
    }
    check("%3$*1$.*2$f/%4$g",18,9,0.1,1.0/3.0);
    check("%2$.*1$e/%2$.*1$g",12,1.0/7.0);
    static const long double extended[]={0.0L,-0.0L,1.0L,-1.5L,0.1L,1.0L/3.0L,0x1.0000000000000002p0L,0xf.fffffffffffffffp-4L,LDBL_MIN,LDBL_MAX,0x0.000000000000001p-16385L};
    static const char *const longspecs[]={"%+.20Lf","%#.21Le","%.21Lg","%La","%.0La","%#.18LA"};
    for (size_t f=0; f<sizeof longspecs/sizeof *longspecs; ++f)
        for (size_t v=0; v<sizeof extended/sizeof *extended; ++v) check_long(longspecs[f],&extended[v]);
    static const long double seventh=1.0L/7.0L;
    unsigned char positional[32];
    int precision=19,number=123;
    memcpy(positional,&precision,4);
    memcpy(positional+4,&seventh,12);
    memcpy(positional+16,&number,4);
    memcpy(positional+20,&extended[4],12);
    check_list("%2$.*1$Lf/%3$d/%4$La",(char *)positional);
    check("%Lf/%Lf/%Lf",0.0L,1.0L,-1.5L);
    check("%Lf %LF %Lg %LG",(long double)INFINITY,(long double)-INFINITY,(long double)NAN,(long double)-NAN);
}
static void strings(void)
{
    check("text/%%/%c/%5c/%-5c",0,'x','y');
    check("%s/%10s/%-10.3s", "alpha", "beta", "gamma");
    check("%s/%.0s/%.3s/%.5s/%.6s/%10s",(char *)0,(char *)0,(char *)0,(char *)0,(char *)0,(char *)0);
    check("%p/%p/%p/%020p/%-12p/%.9p",(void *)0,(void *)1,(void *)(uintptr_t)0xdeadbeef,(void *)0x123,(void *)0,(void *)0x123);
    check("%1$p/%1$20p/%1$.0p",(void *)0);
    check("%lc/%C/%ls/%S",L'\u00e9',L'\u20ac',L"A\u00e9\u20ac\U0001f642",L"\U0001f642Z");
    for (int precision=0; precision<=11; ++precision) check("%12.*ls",precision,L"A\u00e9\u20ac\U0001f642Z");
    check("%2$.*1$ls/%3$d/%4$lc",6,L"A\u00e9\u20ac\U0001f642",77,L'\U0001f642');
    check("%ls/%.3ls/%10ls",(wchar_t *)0,(wchar_t *)0,(wchar_t *)0);
    check_wide(L"wide/%%/%lc/%ls/%s",L'\U0001f642',L"\u00e9\u20ac", "A\xc3\xa9\xe2\x82\xac");
    check_wide(L"%8.2ls/%8.2s/%C/%S",L"\u00e9\u20acZ","\xc3\xa9\xe2\x82\xacZ",L'\u20ac',L"wide");
    check_wide(L"%3$*1$.*2$ls/%4$.4f",8,2,L"\u00e9\u20acZ",1.25);
    wchar_t w[8];
    for (size_t cap=1; cap<=6; ++cap) {
        for (size_t i=0; i<8; ++i) w[i]=0x7777;
        int n=swprintf(w,cap,L"abcdef");
        printf("%03u swprintf-trunc %u %d",row++,(unsigned)cap,n);
        for (size_t i=0; i<8; ++i) printf(" %08x",(unsigned)w[i]);
        putchar('\n');
    }
}
static int call_vasprintf(char **s, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vasprintf(s,fmt,ap);
    va_end(ap); return n;
}
static int call_vsprintf(char *s, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vsprintf(s,fmt,ap);
    va_end(ap); return n;
}
static int call_vfprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vfprintf(f,fmt,ap);
    va_end(ap); return n;
}
static int call_vdprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vdprintf(fd,fmt,ap);
    va_end(ap); return n;
}
static int call_vprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int n=vprintf(fmt,ap);
    va_end(ap); return n;
}
extern int __sprintf_chk(char *,int,size_t,const char *,...);
extern int __snprintf_chk(char *,size_t,int,size_t,const char *,...);
extern int __vsprintf_chk(char *,int,size_t,const char *,va_list);
extern int __vsnprintf_chk(char *,size_t,int,size_t,const char *,va_list);
extern int __fprintf_chk(FILE *,int,const char *,...);
extern int __vfprintf_chk(FILE *,int,const char *,va_list);
extern int __printf_chk(int,const char *,...);
extern int __vprintf_chk(int,const char *,va_list);
extern int __asprintf_chk(char **,int,const char *,...);
extern int __vasprintf_chk(char **,int,const char *,va_list);
extern int __dprintf_chk(int,int,const char *,...);
static void checked_variants(FILE *f, const char *fmt, ...)
{
    char a[128],b[128],*c;
    va_list ap;
    va_start(ap,fmt); int n=__vsprintf_chk(a,1,sizeof a,fmt,ap); va_end(ap);
    report("vsprintf_chk",n,a,(size_t)n);
    va_start(ap,fmt); n=__vsnprintf_chk(b,sizeof b,1,sizeof b,fmt,ap); va_end(ap);
    report("vsnprintf_chk",n,b,(size_t)n);
    va_start(ap,fmt); n=__vasprintf_chk(&c,1,fmt,ap); va_end(ap);
    report("vasprintf_chk",n,c,(size_t)n); free(c);
    va_start(ap,fmt); n=__vfprintf_chk(f,1,fmt,ap); va_end(ap);
    printf("%03u vfprintf_chk %d\n",row++,n);
    va_start(ap,fmt); n=__vprintf_chk(1,fmt,ap); va_end(ap);
    printf(" %03u vprintf_chk %d\n",row++,n);
}
static void wrappers(void)
{
    char out[256],*allocated;
    int n=asprintf(&allocated,"%2$d:%1$s:%3$.4f","allocated",27,0.125);
    report("asprintf",n,allocated,(size_t)n); free(allocated);
    static const long double tenth=0.1L;
    const char *prefix="va";
    unsigned char raw[16];
    memcpy(raw,&prefix,4);
    memcpy(raw+4,&tenth,12);
    n=vasprintf(&allocated,"%s:%La",(char *)raw);
    report("vasprintf",n,allocated,(size_t)n); free(allocated);
    n=call_vasprintf(&allocated,"%s:%.3f","va",0.125);
    report("vasprintf-varargs",n,allocated,(size_t)n); free(allocated);
    n=call_vsprintf(out,"%04d/%s",12,"vsprintf");
    report("vsprintf",n,out,(size_t)n);
    n=sprintf(out,"%04d/%s",12,"sprintf");
    report("sprintf",n,out,(size_t)n);
    for (size_t cap=0; cap<12; ++cap) {
        memset(out,0x5a,sizeof out);
        n=snprintf(out,cap,"[%08x]",0x1234);
        report("snprintf-trunc",n,out,14);
    }
    printf("%03u snprintf-count %d\n",row++,snprintf(NULL,0,"%020d/%s",23,"count"));
    signed char hh=-1; short h=-1; int i=-1; long l=-1; long long ll=-1; intmax_t j=-1; size_t z=0; ptrdiff_t t=-1;
    n=snprintf(out,sizeof out,"abc%hhn/%hn/%n/%ln/%lln/%jn/%zn/%tnEND",&hh,&h,&i,&l,&ll,&j,&z,&t);
    report("counts",n,out,(size_t)n);
    printf("%03u n-values %d %d %d %ld %lld %jd %zu %td\n",row++,hh,h,i,l,ll,j,z,t);
    wchar_t wide[64]; int count=-1;
    n=swprintf(wide,64,L"\u00e9\u20ac%n/%s",&count,"\xc3\xa9");
    printf("%03u wide-count %d %d\n",row++,n,count);
    n=__sprintf_chk(out,1,sizeof out,"%s/%d","sprintf_chk",9);
    report("sprintf_chk",n,out,(size_t)n);
    n=__snprintf_chk(out,8,1,sizeof out,"%s/%d","snprintf_chk",9);
    report("snprintf_chk",n,out,8);
    n=__asprintf_chk(&allocated,1,"%s/%d","asprintf_chk",9);
    report("asprintf_chk",n,allocated,(size_t)n); free(allocated);
    FILE *f=tmpfile();
    if (!f) { puts("tmpfile-failed"); return; }
    int a=fprintf(f,"file:%04d;",12);
    int b=call_vfprintf(f,"vfile:%s;","ok");
    fflush(f);
    int c=dprintf(fileno(f),"fd:%#x;",42);
    int d=call_vdprintf(fileno(f),"vfd:%.2f;",0.5);
    int e=__fprintf_chk(f,1,"chk:%d;",7);
    fflush(f);
    int g=__dprintf_chk(fileno(f),1,"fdchk:%d;",8);
    checked_variants(f,"[%d/%s]",19,"checked");
    fflush(f);
    printf("%03u file-returns %d %d %d %d %d %d\n",row++,a,b,c,d,e,g);
    rewind(f);
    size_t length=fread(out,1,sizeof out,f);
    report("file-bytes",(int)length,out,length);
    fclose(f);
    n=call_vprintf("vprintf:%d/%s",21,"ok");
    printf(" %03u vprintf %d\n",row++,n);
    n=__printf_chk(1,"printf_chk:%d/%s",22,"ok");
    printf(" %03u printf_chk %d\n",row++,n);
}
static void transfer_bytes(const char *name, const void *value)
{
    printf("%s:",name);
    bytes(value,10); /* The last two i386 long-double bytes are padding. */
    putchar('\n');
}
__attribute__((noinline)) static void transfer_parameter(long double value)
{
    transfer_bytes("parameter",&value);
}
static void x87_transfer(void)
{
    static const long double original=0.1L;
    unsigned short control;
    __asm__ volatile("fnstcw %0":"=m"(control));
    printf("control:%04x\n",control);
    transfer_bytes("static",&original);
    volatile const long double *source=&original;
    volatile long double transferred=*source;
    transfer_bytes("transfer",(const void *)&transferred);
    transfer_parameter(*source);
    unsigned char a[12]={0},b[12]={0};
    __asm__ volatile("fldt %2; fld %%st(0); fstpt %0; fstpt %1"
        :"=m"(a),"=m"(b):"m"(original):"st");
    transfer_bytes("duplicate1",a);
    transfer_bytes("duplicate2",b);
    check_long("%.21Lg",&original);
    check_long("%La",&original);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL,"C.UTF-8");
    if (argc>1 && !strcmp(argv[1],"x87-transfer")) {
        x87_transfer(); return 0;
    }
    /* Separate invocations allow the parent to check fortify termination. */
    if (argc>1 && !strcmp(argv[1],"overflow-sprintf")) {
        char out[4]; __sprintf_chk(out,1,sizeof out,"%s","overflow"); return 99;
    }
    if (argc>1 && !strcmp(argv[1],"overflow-snprintf")) {
        char out[4]; __snprintf_chk(out,8,1,sizeof out,"%s","overflow"); return 99;
    }
    integers(); floating(); strings();
    static const int errors[]={0,ENOENT,EINVAL,EILSEQ,EOVERFLOW,12345,-1};
    for (size_t i=0; i<sizeof errors/sizeof *errors; ++i) {
        errno=errors[i]; check("%m/%20.12m/%1$d/%m",17);
    }
    wrappers();
    return 0;
}
