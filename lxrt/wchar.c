/* Guest wchar_t is UCS-4, not the native Windows UTF-16 wchar_t.
 * C rejects bytes/scalars above ASCII with EILSEQ. C.UTF-8 follows glibc's
 * extended 1..6-byte multibyte encoding through U+7FFFFFFF (no surrogates).
 * The stateless UTF-8 helpers used by iconv/stdio instead enforce Unicode's
 * U+10FFFF ceiling. Width uses Windows combining data and explicit East Asian
 * ranges; Unicode-version edge cases may differ from glibc's width tables.
 * wcstold[_l] returns via x87 ST0 but parses at double precision. */
#include "lxlocale.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

size_t lx_wcslen(const uint32_t *s) { const uint32_t *p = s; while (*p) ++p; return (size_t)(p-s); }
static size_t wcsnlen_(const uint32_t *s, size_t n) { size_t i=0; while(i<n && s[i]) ++i; return i; }
int lx_wcscmp(const uint32_t *a, const uint32_t *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a > *b ? 1 : *a < *b ? -1 : 0;
}
static int wcsncmp_(const uint32_t *a, const uint32_t *b, size_t n)
{
    while(n && *a && *a==*b) {++a;++b;--n;}
    return !n ? 0 : *a > *b ? 1 : *a < *b ? -1 : 0;
}
static uint32_t *wcscpy_(uint32_t *d, const uint32_t *s) { uint32_t *r=d; while((*d++=*s++)) {} return r; }
static uint32_t *wcsncpy_(uint32_t *d, const uint32_t *s, size_t n)
{ uint32_t *r=d; while(n && *s) {*d++=*s++;--n;} while(n--) *d++=0; return r; }
static uint32_t *wcpcpy_(uint32_t *d, const uint32_t *s) { while((*d=*s)) {++d;++s;} return d; }
static uint32_t *wcpncpy_(uint32_t *d, const uint32_t *s, size_t n)
{ size_t len=wcsnlen_(s,n); wcsncpy_(d,s,n); return d+len; }
static uint32_t *wcscat_(uint32_t *d, const uint32_t *s) { wcscpy_(d+lx_wcslen(d),s); return d; }
static uint32_t *wcsncat_(uint32_t *d, const uint32_t *s, size_t n)
{ uint32_t *p=d+lx_wcslen(d); while(n-- && *s) *p++=*s++; *p=0; return d; }
static uint32_t *wcschr_(const uint32_t *s, uint32_t c) { do { if(*s==c) return (uint32_t *)s; } while(*s++); return NULL; }
static uint32_t *wcsrchr_(const uint32_t *s, uint32_t c) { const uint32_t *r=NULL; do {if(*s==c) r=s;} while(*s++); return (uint32_t *)r; }
static uint32_t *wcsstr_(const uint32_t *s, const uint32_t *find)
{ size_t n=lx_wcslen(find); if(!n) return (uint32_t *)s; for(;*s;++s) if(!wcsncmp_(s,find,n)) return (uint32_t *)s; return NULL; }
static uint32_t *wcsdup_(const uint32_t *s) {size_t n=(lx_wcslen(s)+1)*4; uint32_t *r=lx_malloc(n); if(r) memcpy(r,s,n); return r;}
size_t lx_wcsxfrm(uint32_t *d, const uint32_t *s, size_t n)
{ size_t len=lx_wcslen(s); if(n) memcpy(d,s,(len<n?len+1:n)*4); return len; }
static uint32_t *wmemchr_(const uint32_t *s, uint32_t c, size_t n) {while(n--) {if(*s==c) return (uint32_t *)s;++s;} return NULL;}
static int wmemcmp_(const uint32_t *a, const uint32_t *b, size_t n)
{while(n--) {if(*a!=*b) return *a>*b?1:-1;++a;++b;} return 0;}
static uint32_t *wmemcpy_(uint32_t *d, const uint32_t *s, size_t n) {return memcpy(d,s,n*4);}
static uint32_t *wmemmove_(uint32_t *d, const uint32_t *s, size_t n) {return memmove(d,s,n*4);}
static uint32_t *wmemset_(uint32_t *d, uint32_t c, size_t n) {uint32_t *r=d;while(n--) *d++=c;return r;}
static size_t wcsspn_(const uint32_t *s,const uint32_t *set) {size_t n=0;while(s[n]&&wcschr_(set,s[n])) ++n;return n;}
static size_t wcscspn_(const uint32_t *s,const uint32_t *set) {size_t n=0;while(s[n]&&!wcschr_(set,s[n])) ++n;return n;}
static uint32_t *wcspbrk_(const uint32_t *s,const uint32_t *set) {s+=wcscspn_(s,set);return *s?(uint32_t *)s:NULL;}
static uint32_t *wcstok_(uint32_t *s,const uint32_t *set,uint32_t **save)
{if(!s) s=*save;if(!s) return NULL;s+=wcsspn_(s,set);if(!*s) {*save=NULL;return NULL;}uint32_t *end=s+wcscspn_(s,set);if(*end) *end++=0;else end=NULL;*save=end;return s;}

int lx_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    if(!n) return -2;
    unsigned c=s[0], need; uint32_t value;
    if(c<0x80) {*cp=c;return 1;}
    if(c>=0xc2&&c<=0xdf) {need=2;value=c&31;}
    else if(c>=0xe0&&c<=0xef) {need=3;value=c&15;}
    else if(c>=0xf0&&c<=0xf4) {need=4;value=c&7;}
    else return -1;
    for(unsigned i=1;i<need;++i) {
        if(i>=n) return -2;
        if((s[i]&0xc0)!=0x80) return -1;
        value=(value<<6)|(s[i]&63);
        if(i==1&&((c==0xe0&&s[i]<0xa0)||(c==0xed&&s[i]>=0xa0)||(c==0xf0&&s[i]<0x90)||(c==0xf4&&s[i]>=0x90))) return -1;
    }
    *cp=value;return (int)need;
}
int lx_utf8_encode(uint32_t cp, char *s)
{
    if(cp<0x80) {s[0]=(char)cp;return 1;}
    if(cp<0x800) {s[0]=(char)(0xc0|(cp>>6));s[1]=(char)(0x80|(cp&63));return 2;}
    if(cp>=0xd800&&cp<=0xdfff) return -1;
    if(cp<0x10000) {s[0]=(char)(0xe0|(cp>>12));s[1]=(char)(0x80|((cp>>6)&63));s[2]=(char)(0x80|(cp&63));return 3;}
    if(cp<=0x10ffff) {s[0]=(char)(0xf0|(cp>>18));s[1]=(char)(0x80|((cp>>12)&63));s[2]=(char)(0x80|((cp>>6)&63));s[3]=(char)(0x80|(cp&63));return 4;}
    return -1;
}
static __declspec(thread) lx_mbstate decode_state, encode_state;
size_t lx_mbrtowc(uint32_t *wc, const char *s, size_t n, lx_mbstate *state)
{
    if(!state) state=&decode_state;
    if(!s) {s="";n=1;wc=NULL;}
    if(!n) return (size_t)-2;
    if(!lx_locale_utf8(NULL)) {
        unsigned c=(unsigned char)*s;
        if(c>=128) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
        state->count=state->value=0;if(wc) *wc=c;return c?1:0;
    }
    unsigned need=state->count&255, seen=(state->count>>8)&255;
    uint32_t value=state->value; size_t used=0;
    if(!need) {
        unsigned c=(unsigned char)s[used++];
        if(c<128) {if(wc) *wc=c;return c?1:0;}
        if(c>=0xc2&&c<=0xdf) {need=2;value=c&31;}
        else if(c>=0xe0&&c<=0xef) {need=3;value=c&15;}
        else if(c>=0xf0&&c<=0xf7) {need=4;value=c&7;}
        else if(c>=0xf8&&c<=0xfb) {need=5;value=c&3;}
        else if(c>=0xfc&&c<=0xfd) {need=6;value=c&1;}
        else {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
        seen=1;
    }
    while(seen<need&&used<n) {
        unsigned c=(unsigned char)s[used];
        if((c&0xc0)!=0x80) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
        value=(value<<6)|(c&63);++seen;++used;
    }
    if(seen<need) {state->count=need|(seen<<8);state->value=value;return (size_t)-2;}
    static const uint32_t minimum[]={0,0,0x80,0x800,0x10000,0x200000,0x4000000};
    if(value<minimum[need] || (value>=0xd800&&value<=0xdfff)) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
    state->count=state->value=0;if(wc) *wc=value;return used;
}
size_t lx_wcrtomb(char *s, uint32_t wc, lx_mbstate *state)
{
    char tmp[6];if(!state) state=&encode_state;
    if(!s) {s=tmp;wc=0;}
    state->count=state->value=0;
    if(!lx_locale_utf8(NULL)) {if(wc>=128) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}*s=(char)wc;return 1;}
    if(wc>=0xd800&&wc<=0xdfff) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
    if(wc<0x80) {*s=(char)wc;return 1;}
    unsigned len=wc<0x800?2:wc<0x10000?3:wc<0x200000?4:wc<0x4000000?5:wc<=0x7fffffff?6:0;
    if(!len) {lx_set_errno(LX_EILSEQ);return (size_t)-1;}
    uint32_t value=wc;
    for(unsigned i=len-1;i;--i) {s[i]=(char)(0x80|(value&63));value>>=6;}
    static const unsigned lead[]={0,0,0xc0,0xe0,0xf0,0xf8,0xfc};s[0]=(char)(lead[len]|value);return len;
}
static size_t mbrlen_(const char *s,size_t n,lx_mbstate *state)
{static __declspec(thread) lx_mbstate implicit;return lx_mbrtowc(NULL,s,n,state?state:&implicit);}
static int mbsinit_(const lx_mbstate *state) {return !state||!state->count;}
static int mbtowc_(uint32_t *wc,const char *s,size_t n)
{static __declspec(thread) lx_mbstate state;if(!s) {memset(&state,0,sizeof(state));return 0;}size_t r=lx_mbrtowc(wc,s,n,&state);return r==(size_t)-2?-1:(int)r;}
static int mblen_(const char *s,size_t n)
{static __declspec(thread) lx_mbstate state;if(!s) {memset(&state,0,sizeof(state));return 0;}size_t r=lx_mbrtowc(NULL,s,n,&state);return r==(size_t)-2?-1:(int)r;}
static int wctomb_(char *s,uint32_t wc) {if(!s) return 0;return (int)lx_wcrtomb(s,wc,NULL);}
static uint32_t btowc_(int c) {return c>=0&&c<128?(uint32_t)c:UINT32_MAX;}
static int wctob_(uint32_t c) {return c<128?(int)c:-1;}
static size_t mbsnrtowcs_(uint32_t *dst,const char **src,size_t bytes,size_t len,lx_mbstate *state)
{
    static __declspec(thread) lx_mbstate implicit;if(!state) state=&implicit;
    lx_mbstate copy=*state;if(!dst) state=&copy;
    const char *p=*src;size_t result=0;
    while(bytes && (!dst||result<len)) {
        uint32_t cp;size_t r=lx_mbrtowc(&cp,p,bytes,state);
        if(r==(size_t)-1) {if(dst) *src=p;return r;}
        if(r==(size_t)-2) {p+=bytes;bytes=0;break;}
        if(!r) {if(dst) {dst[result]=0;*src=NULL;}return result;}
        if(dst) dst[result]=cp;++result;p+=r;bytes-=r;
    }
    if(dst) *src=p;return result;
}
static size_t mbsrtowcs_(uint32_t *dst,const char **src,size_t len,lx_mbstate *state)
{static __declspec(thread) lx_mbstate implicit;return mbsnrtowcs_(dst,src,SIZE_MAX,len,state?state:&implicit);}
static size_t mbstowcs_(uint32_t *dst,const char *src,size_t len) {lx_mbstate state={0};return mbsrtowcs_(dst,&src,len,&state);}
static size_t wcsnrtombs_(char *dst,const uint32_t **src,size_t chars,size_t len,lx_mbstate *state)
{
    static __declspec(thread) lx_mbstate implicit;if(!state) state=&implicit;
    lx_mbstate copy=*state;if(!dst) state=&copy;
    const uint32_t *p=*src;size_t result=0;
    while(chars && (!dst||result<len)) {
        char encoded[6];size_t r=lx_wcrtomb(encoded,*p,state);
        if(r==(size_t)-1) {if(dst) *src=p;return r;}
        if(dst&&r>len-result) break;
        if(dst) memcpy(dst+result,encoded,r);
        if(!*p) {if(dst) *src=NULL;return result;}
        result+=r;++p;--chars;
    }
    if(dst) *src=p;return result;
}
static size_t wcsrtombs_(char *dst,const uint32_t **src,size_t len,lx_mbstate *state)
{static __declspec(thread) lx_mbstate implicit;return wcsnrtombs_(dst,src,SIZE_MAX,len,state?state:&implicit);}
static size_t wcstombs_(char *dst,const uint32_t *src,size_t len) {lx_mbstate state={0};return wcsrtombs_(dst,&src,len,&state);}

static int wide_space(uint32_t c)
{if(c==' '||(c>=9&&c<=13)) return 1;if(!lx_locale_utf8(NULL)||c>0xffff) return 0;wchar_t w=(wchar_t)c;WORD flags;return GetStringTypeW(CT_CTYPE1,&w,1,&flags)&&(flags&C1_SPACE);}
static uint64_t wide_integer(const uint32_t *s,uint32_t **end,int base,int unsig,int bits)
{
    const uint32_t *start=s;while(wide_space(*s)) ++s;int neg=*s=='-';if(*s=='+'||*s=='-') ++s;
    if(base&& (base<2||base>36)) {lx_set_errno(LX_EINVAL);if(end) *end=(uint32_t *)start;return 0;}
    if((!base||base==16)&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')&&((s[2]>='0'&&s[2]<='9')||(s[2]>='a'&&s[2]<='f')||(s[2]>='A'&&s[2]<='F'))) {base=16;s+=2;}
    if(!base) base=*s=='0'?8:10;
    uint64_t limit=bits==32?(unsig?UINT32_MAX:neg?2147483648ULL:2147483647ULL):(unsig?UINT64_MAX:neg?0x8000000000000000ULL:0x7fffffffffffffffULL);
    uint64_t value=0, cutoff=limit/(unsigned)base; unsigned cutlim=(unsigned)(limit%(unsigned)base);
    int any=0,overflow=0;
    for(;;++s) {uint32_t c=*s;unsigned digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='z'?c-'a'+10:c>='A'&&c<='Z'?c-'A'+10:99;if(digit>=(unsigned)base) break;any=1;if(value>cutoff||(value==cutoff&&digit>cutlim)) overflow=1;else value=value*(unsigned)base+digit;}
    if(end) *end=(uint32_t *)(any?s:start);
    if(overflow) {lx_set_errno(LX_ERANGE);return !unsig&&neg?(uint64_t)-limit:limit;}
    return neg?(uint64_t)-value:value;
}
static long wcstol_(const uint32_t *s,uint32_t **e,int b) {return (long)wide_integer(s,e,b,0,32);}
static unsigned long wcstoul_(const uint32_t *s,uint32_t **e,int b) {return (unsigned long)wide_integer(s,e,b,1,32);}
static int64_t wcstoll_(const uint32_t *s,uint32_t **e,int b) {return (int64_t)wide_integer(s,e,b,0,64);}
static uint64_t wcstoull_(const uint32_t *s,uint32_t **e,int b) {return wide_integer(s,e,b,1,64);}
static double wide_float(const uint32_t *s,uint32_t **end,int single)
{
    const uint32_t *start=s;while(wide_space(*s)) ++s;
    size_t n=0;while(s[n]&&s[n]<128) ++n;
    char local[256],*text=n<sizeof(local)?local:malloc(n+1);
    if(!text) {lx_set_errno(LX_ENOMEM);if(end) *end=(uint32_t *)start;return 0;}
    for(size_t i=0;i<n;++i) text[i]=(char)s[i];text[n]=0;
    char *stop;errno=0;double result=single?(double)strtof(text,&stop):strtod(text,&stop);if(errno==ERANGE) lx_set_errno(LX_ERANGE);
    if(end) *end=(uint32_t *)(stop==text?start:s+(stop-text));
    if(text!=local) free(text);return result;
}
static double wcstod_(const uint32_t *s,uint32_t **end) {return wide_float(s,end,0);}
static float wcstof_(const uint32_t *s,uint32_t **end) {return (float)wide_float(s,end,1);}
static __declspec(naked) void wcstold_(void) {__asm {jmp wcstod_}}
static long wcstol_l_(const uint32_t *s,uint32_t **e,int b,lx_locale *l) {return wcstol_(s,e,b);}
static unsigned long wcstoul_l_(const uint32_t *s,uint32_t **e,int b,lx_locale *l) {return wcstoul_(s,e,b);}
static double wcstod_l_(const uint32_t *s,uint32_t **e,lx_locale *l) {return wcstod_(s,e);}
static float wcstof_l_(const uint32_t *s,uint32_t **e,lx_locale *l) {return wcstof_(s,e);}
static int wcwidth_(uint32_t c)
{
    if(!c) return 0;
    if(c<32||(c>=0x7f&&c<0xa0)||c>0x10ffff||(c>=0xd800&&c<=0xdfff)) return -1;
    if(!lx_locale_utf8(NULL)) return c<127?1:-1;
    if((c>=0x1160&&c<=0x11ff)||(c>=0x200b&&c<=0x200f)||(c>=0x202a&&c<=0x202e)||(c>=0x2060&&c<=0x2064)||(c>=0x2066&&c<=0x206f)||c==0xfeff||(c>=0xfe00&&c<=0xfe0f)||(c>=0xe0100&&c<=0xe01ef)) return 0;
    if(c<=0xffff) {wchar_t w=(wchar_t)c;WORD flags;if(GetStringTypeW(CT_CTYPE3,&w,1,&flags)&&(flags&(C3_NONSPACING|C3_VOWELMARK))) return 0;}
    if(c>=0x1100&&(c<=0x115f||c==0x2329||c==0x232a||(c>=0x2e80&&c<=0xa4cf&&c!=0x303f)||(c>=0xac00&&c<=0xd7a3)||(c>=0xf900&&c<=0xfaff)||(c>=0xfe10&&c<=0xfe19)||(c>=0xfe30&&c<=0xfe6f)||(c>=0xff00&&c<=0xff60)||(c>=0xffe0&&c<=0xffe6)||(c>=0x1f300&&c<=0x1f64f)||(c>=0x1f900&&c<=0x1f9ff)||(c>=0x20000&&c<=0x3fffd))) return 2;
    return 1;
}
static int wcswidth_(const uint32_t *s,size_t n) {int result=0;while(n--&&*s) {int w=wcwidth_(*s++);if(w<0) return -1;result+=w;}return result;}
extern size_t lx_wcsftime(uint32_t *,size_t,const uint32_t *,const void *);
#define BOTH(n,f) LX_SYM(n,f),LX_SYM("__" n,f)
LX_SYMTAB(wchar,
    LX_SYM("wcslen",lx_wcslen),LX_SYM("wcsnlen",wcsnlen_),LX_SYM("wcscmp",lx_wcscmp),LX_SYM("wcsncmp",wcsncmp_),LX_SYM("wcscpy",wcscpy_),LX_SYM("wcsncpy",wcsncpy_),LX_SYM("wcpcpy",wcpcpy_),LX_SYM("wcpncpy",wcpncpy_),LX_SYM("wcscat",wcscat_),LX_SYM("wcsncat",wcsncat_),LX_SYM("wcschr",wcschr_),LX_SYM("wcsrchr",wcsrchr_),LX_SYM("wcsstr",wcsstr_),LX_SYM("wcsdup",wcsdup_),LX_SYM("wcscoll",lx_wcscmp),LX_SYM("wcsxfrm",lx_wcsxfrm),LX_SYM("wcsftime",lx_wcsftime),
    LX_SYM("wmemchr",wmemchr_),LX_SYM("wmemcmp",wmemcmp_),LX_SYM("wmemcpy",wmemcpy_),LX_SYM("wmemmove",wmemmove_),LX_SYM("wmemset",wmemset_),LX_SYM("wcsspn",wcsspn_),LX_SYM("wcscspn",wcscspn_),LX_SYM("wcspbrk",wcspbrk_),LX_SYM("wcstok",wcstok_),
    BOTH("mbrtowc",lx_mbrtowc),LX_SYM("mbrlen",mbrlen_),LX_SYM("mbsinit",mbsinit_),LX_SYM("mbtowc",mbtowc_),LX_SYM("mblen",mblen_),LX_SYM("mbstowcs",mbstowcs_),LX_SYM("mbsrtowcs",mbsrtowcs_),LX_SYM("mbsnrtowcs",mbsnrtowcs_),BOTH("wcrtomb",lx_wcrtomb),LX_SYM("wctomb",wctomb_),LX_SYM("wcstombs",wcstombs_),LX_SYM("wcsrtombs",wcsrtombs_),LX_SYM("wcsnrtombs",wcsnrtombs_),LX_SYM("btowc",btowc_),LX_SYM("wctob",wctob_),LX_SYM("wcwidth",wcwidth_),LX_SYM("wcswidth",wcswidth_),
    LX_SYM("wcstol",wcstol_),LX_SYM("wcstoul",wcstoul_),LX_SYM("wcstoll",wcstoll_),LX_SYM("wcstoull",wcstoull_),LX_SYM("wcstod",wcstod_),LX_SYM("wcstof",wcstof_),LX_SYM("wcstold",wcstold_),BOTH("wcstol_l",wcstol_l_),BOTH("wcstoul_l",wcstoul_l_),BOTH("wcstod_l",wcstod_l_),BOTH("wcstof_l",wcstof_l_),BOTH("wcstold_l",wcstold_)
);
