#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

_Static_assert(sizeof(struct __locale_struct)==116,"i386 locale_t");
_Static_assert(offsetof(struct __locale_struct,__ctype_b)==52,"ctype field");
_Static_assert(offsetof(struct __locale_struct,__names)==64,"names field");
_Static_assert(sizeof(struct lconv)==56,"i386 lconv");
_Static_assert(sizeof(mbstate_t)==8,"i386 mbstate_t");
_Static_assert(sizeof(wchar_t)==4,"guest UCS4");
#define ITEM(x) {#x,x}
static const struct {const char *name;nl_item item;} strings[]={
    ITEM(CODESET),ITEM(RADIXCHAR),ITEM(THOUSEP),ITEM(GROUPING),
    ITEM(ABDAY_1),ITEM(DAY_7),ITEM(ABMON_1),ITEM(MON_12),ITEM(AM_STR),ITEM(PM_STR),
    ITEM(D_T_FMT),ITEM(D_FMT),ITEM(T_FMT),ITEM(T_FMT_AMPM),ITEM(ERA),ITEM(ERA_D_FMT),ITEM(ERA_D_T_FMT),ITEM(ERA_T_FMT),
    ITEM(INT_CURR_SYMBOL),ITEM(CURRENCY_SYMBOL),ITEM(MON_DECIMAL_POINT),ITEM(MON_THOUSANDS_SEP),ITEM(MON_GROUPING),ITEM(POSITIVE_SIGN),ITEM(NEGATIVE_SIGN),
    ITEM(YESEXPR),ITEM(NOEXPR),ITEM(_DATE_FMT),ITEM(_NL_TIME_CODESET)
};
static const struct {const char *name;nl_item item;} wide[]={ITEM(_NL_WABDAY_1),ITEM(_NL_WDAY_7),ITEM(_NL_WABMON_1),ITEM(_NL_WMON_12),ITEM(_NL_WAM_STR),ITEM(_NL_WD_T_FMT),ITEM(_NL_WD_FMT),ITEM(_NL_WT_FMT),ITEM(_NL_WT_FMT_AMPM),ITEM(_NL_WERA_D_FMT)};
static const struct {const char *name;nl_item item;} numbers[]={ITEM(_NL_CTYPE_MB_CUR_MAX),ITEM(_NL_NUMERIC_DECIMAL_POINT_WC),ITEM(_NL_NUMERIC_THOUSANDS_SEP_WC),ITEM(_NL_MONETARY_DECIMAL_POINT_WC),ITEM(_NL_MONETARY_THOUSANDS_SEP_WC),ITEM(_NL_CTYPE_OUTDIGIT0_WC),ITEM(_NL_CTYPE_INDIGITS_WC_LEN)};
static void printwide(const wchar_t *s) {while(*s) putchar((char)*s++);}
int main(void)
{
    setlocale(LC_ALL,"C");locale_t c=newlocale(LC_ALL_MASK,"C",0),d=duplocale(c);
    locale_t old=uselocale(d);
    printf("locale %u %u %d %d %d\n",(unsigned)sizeof(*c),(unsigned)sizeof(struct lconv),c->__ctype_b['A']==(*__ctype_b_loc())['A'],c->__ctype_tolower['A'],c->__ctype_toupper['a']);
    printf("thread %d %d\n",old==LC_GLOBAL_LOCALE,uselocale(0)==d);
    locale_t allbit=newlocale(1 << LC_ALL,"C",0);
    printf("all-category-bit %d\n",allbit!=NULL);
    if(!allbit) return 1;
    freelocale(allbit);
    for(unsigned i=0;i<sizeof(strings)/sizeof(*strings);++i) printf("%s %x [%s]\n",strings[i].name,strings[i].item,nl_langinfo_l(strings[i].item,c));
    for(unsigned i=0;i<sizeof(wide)/sizeof(*wide);++i) {printf("%s %x [",wide[i].name,wide[i].item);printwide((wchar_t *)nl_langinfo_l(wide[i].item,c));puts("]");}
    for(unsigned i=0;i<sizeof(numbers)/sizeof(*numbers);++i) printf("%s %x %u\n",numbers[i].name,numbers[i].item,(unsigned)(uintptr_t)nl_langinfo_l(numbers[i].item,c));
    printf("monetary %u %u %u %u\n",(unsigned char)*nl_langinfo_l(INT_FRAC_DIGITS,c),(unsigned char)*nl_langinfo_l(P_CS_PRECEDES,c),(unsigned char)*nl_langinfo_l(INT_P_CS_PRECEDES,c),(unsigned char)*nl_langinfo_l(INT_N_SIGN_POSN,c));
    struct lconv *lc=localeconv();printf("lconv [%s] [%s] %u %u\n",lc->decimal_point,lc->thousands_sep,(unsigned char)lc->frac_digits,(unsigned char)lc->int_n_sign_posn);
    struct tm tm={.tm_sec=6,.tm_min=5,.tm_hour=16,.tm_mday=3,.tm_mon=1,.tm_year=124,.tm_wday=6,.tm_yday=33};
    char buf[128];wchar_t wbuf[128];size_t a=strftime_l(buf,sizeof(buf),"%c %F %r",&tm,c);size_t b=wcsftime_l(wbuf,128,L"%c %F %r",&tm,c);
    printf("strftime %u %s\n",(unsigned)a,buf);printf("wcsftime %u ",(unsigned)b);printwide(wbuf);puts("");
    errno=0;locale_t bad=newlocale(LC_ALL_MASK,"unknown_LXRT",0);printf("unknown %d %d\n",bad==0,errno);
    uselocale(old);freelocale(d);freelocale(c);return 0;
}
