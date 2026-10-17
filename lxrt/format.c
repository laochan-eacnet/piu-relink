/* Guest i386 printf: decode 4/8/12-byte arguments in two passes, including
 * positional width/precision. Numeric locale is C/C.UTF-8 (no grouping).
 * Binary64 decimals use one rebuilt UCRT numeric spec with C99 rounding;
 * hexadecimal and x87 decimals retain their original binary significands.
 * Wide buffers/counts use guest 32-bit wchar_t and UTF-8, never host wchar_t.
 * Output beyond INT_MAX fails with Linux EOVERFLOW; fortify bounds abort.
 * As with glibc, conflicting positional types/invalid argument pointers are
 * undefined input, not a separate validation or recovery protocol. */
#include "lxstdio.h"
#include <corecrt_stdio_config.h>
/* UCRT otherwise retains its pre-C99 rounding mode for compatibility. This is
 * translation-unit local: never mutate the CRT's process-wide option storage. */
#undef _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS
#define _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS _CRT_INTERNAL_PRINTF_STANDARD_ROUNDING
#include <stdio.h>
#include <string.h>
#include <limits.h>

enum { F_LEFT=1, F_PLUS=2, F_SPACE=4, F_ALT=8, F_ZERO=16, F_GROUP=32 };
enum length { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_CAPL, LEN_J, LEN_Z, LEN_T };
enum argtype { ARG_NONE, ARG_WORD, ARG_QUAD, ARG_DOUBLE, ARG_EXTENDED };
typedef struct { unsigned char type; const unsigned char *p; } argument;
typedef struct {
    const char *end;
    int flags, width, precision, index, width_index, precision_index;
    enum length length;
    unsigned char conversion;
} conversion;
typedef struct {
    lx_format_sink sink;
    void *ctx;
    int count, wide;
} output;
static int wide_sink(void *, const char *, size_t);

static int number(const char **s, int *v)
{
    unsigned n=0;
    while (**s >= '0' && **s <= '9') {
        unsigned digit=(unsigned)(*(*s)++-'0');
        if (n > ((unsigned)INT_MAX-digit)/10) return lx_set_errno(LX_EOVERFLOW);
        n=n*10+digit;
    }
    *v=(int)n;
    return 0;
}

static int parse(const char *s, int *next, conversion *c)
{
    memset(c, 0, sizeof *c);
    c->precision=-1;
    const char *p=s;
    int position=0;
    if (*p >= '0' && *p <= '9') {
        if (number(&p, &position)<0) return -1;
        if (*p=='$') {
            if (!position) return lx_set_errno(LX_EINVAL);
            c->index=position;
            s=p+1;
        }
    }
    for (;;) {
        int flag=0;
        switch (*s) {
        case '-': flag=F_LEFT; break;
        case '+': flag=F_PLUS; break;
        case ' ': flag=F_SPACE; break;
        case '#': flag=F_ALT; break;
        case '0': flag=F_ZERO; break;
        case '\'': flag=F_GROUP; break;
        }
        if (!flag) break;
        c->flags|=flag;
        ++s;
    }
    if (*s=='*') {
        ++s;
        p=s;
        position=0;
        if (number(&p, &position)<0) return -1;
        if (*p=='$') {
            if (!position) return lx_set_errno(LX_EINVAL);
            c->width_index=position;
            s=p+1;
        } else c->width_index=(*next)++;
    } else if (number(&s, &c->width)<0) return -1;
    if (*s=='.') {
        ++s;
        c->precision=0;
        if (*s=='*') {
            ++s;
            p=s;
            position=0;
            if (number(&p, &position)<0) return -1;
            if (*p=='$') {
                if (!position) return lx_set_errno(LX_EINVAL);
                c->precision_index=position;
                s=p+1;
            } else c->precision_index=(*next)++;
        } else if (number(&s, &c->precision)<0) return -1;
    }
    switch (*s) {
    case 'h': c->length=LEN_H; if (*++s=='h') { c->length=LEN_HH; ++s; } break;
    case 'l': c->length=LEN_L; if (*++s=='l') { c->length=LEN_LL; ++s; } break;
    case 'q': c->length=LEN_LL; ++s; break;
    case 'L': c->length=LEN_CAPL; ++s; break;
    case 'j': c->length=LEN_J; ++s; break;
    case 'z': case 'Z': c->length=LEN_Z; ++s; break;
    case 't': c->length=LEN_T; ++s; break;
    }
    if (!*s) return lx_set_errno(LX_EINVAL);
    c->conversion=(unsigned char)*s++;
    c->end=s;
    if (strchr("diuoxXcCsSpneEfFgGaA", c->conversion) && !c->index)
        c->index=(*next)++;
    return 0;
}

static unsigned argument_type(const conversion *c)
{
    if (strchr("eEfFgGaA", c->conversion))
        return c->length==LEN_CAPL ? ARG_EXTENDED : ARG_DOUBLE;
    if (strchr("diuoxX", c->conversion) &&
        (c->length==LEN_LL || c->length==LEN_CAPL || c->length==LEN_J)) return ARG_QUAD;
    return ARG_WORD;
}

static int register_argument(argument **args, size_t *cap, argument *local, int index, unsigned type)
{
    if (!index) return 0;
    if ((size_t)index >= *cap) {
        size_t needed=(size_t)index+1, grown=*cap;
        if (needed > SIZE_MAX/sizeof **args) return lx_set_errno(LX_ENOMEM);
        while (grown<needed) {
            if (grown > SIZE_MAX/2) { grown=needed; break; }
            grown*=2;
        }
        if (grown > SIZE_MAX/sizeof **args) grown=needed;
        argument *a=lx_malloc(grown*sizeof *a);
        if (!a) return lx_set_errno(LX_ENOMEM);
        memcpy(a, *args, *cap*sizeof *a);
        memset(a+*cap, 0, (grown-*cap)*sizeof *a);
        if (*args!=local) lx_free(*args);
        *args=a;
        *cap=grown;
    }
    (*args)[index].type=(unsigned char)type;
    return 0;
}

static uint32_t word(const argument *a)
{
    uint32_t n;
    memcpy(&n, a->p, sizeof n);
    return n;
}
static uint64_t quad(const argument *a)
{
    uint64_t n;
    memcpy(&n, a->p, sizeof n);
    return n;
}
static int emit(output *o, const char *s, size_t n)
{
    size_t count=n;
    if (!n) return 0;
    if (o->wide) {
        count=0;
        for (size_t i=0; i<n;) {
            uint32_t wc;
            int z=lx_utf8_decode((const unsigned char *)s+i, n-i, &wc);
            if (z<=0) return lx_set_errno(LX_EILSEQ);
            i+=(size_t)z;
            ++count;
        }
    }
    if (count>(size_t)(INT_MAX-o->count)) return lx_set_errno(LX_EOVERFLOW);
    if (o->sink(o->ctx, s, n)<0) return -1;
    o->count+=(int)count;
    return 0;
}
static int repeat(output *o, char c, size_t n)
{
    char buf[128];
    memset(buf, c, sizeof buf);
    while (n) {
        size_t z=n<sizeof buf ? n : sizeof buf;
        if (emit(o, buf, z)<0) return -1;
        n-=z;
    }
    return 0;
}
static int field(output *o, const conversion *c, const char *prefix, size_t pn,
                 const char *body, size_t bn, size_t units, size_t zeroes, int numeric)
{
    size_t len=pn+units+zeroes;
    size_t pad=c->width>0 && (size_t)c->width>len ? (size_t)c->width-len : 0;
    int zero=numeric && (c->flags&F_ZERO) && !(c->flags&F_LEFT);
    if (!(c->flags&F_LEFT) && !zero && repeat(o, ' ', pad)<0) return -1;
    if (emit(o, prefix, pn)<0) return -1;
    if (zero && repeat(o, '0', pad)<0) return -1;
    if (repeat(o, '0', zeroes)<0 || emit(o, body, bn)<0) return -1;
    if ((c->flags&F_LEFT) && repeat(o, ' ', pad)<0) return -1;
    return 0;
}
static size_t unsigned_digits(char *end, uint64_t n, unsigned base, int upper)
{
    const char *digits=upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p=end;
    do { *--p=digits[n%base]; n/=base; } while (n);
    return (size_t)(end-p);
}
static int integer(output *o, conversion *c, const argument *a)
{
    int sign_conversion=c->conversion=='d' || c->conversion=='i';
    int pointer=c->conversion=='p';
    uint64_t v=(c->length==LEN_LL || c->length==LEN_CAPL || c->length==LEN_J) && !pointer ? quad(a) : word(a);
    int negative=0;
    if (sign_conversion) {
        int64_t s;
        switch (c->length) {
        case LEN_HH: s=(int8_t)v; break;
        case LEN_H: s=(int16_t)v; break;
        case LEN_LL: case LEN_CAPL: case LEN_J: s=(int64_t)v; break;
        default: s=(int32_t)v; break;
        }
        negative=s<0;
        v=negative ? 0-(uint64_t)s : (uint64_t)s;
    } else if (!pointer) {
        if (c->length==LEN_HH) v=(uint8_t)v;
        if (c->length==LEN_H) v=(uint16_t)v;
    }
    if (pointer && !v) return field(o, c, "", 0, "(nil)", 5, 5, 0, 0);
    unsigned base=c->conversion=='o' ? 8 : (pointer || c->conversion=='x' || c->conversion=='X') ? 16 : 10;
    char buf[65], prefix[3];
    /* Only fixed native arguments reach UCRT, never the guest va_list.
     * Prefix/precision padding remains here for glibc's pointer semantics. */
    char spec[] = "%llu";
    spec[3] = base == 8 ? 'o' : base == 16 ? (c->conversion == 'X' ? 'X' : 'x') : 'u';
    int digits = snprintf(buf, sizeof buf, spec, (unsigned long long)v);
    if (digits < 0) return -1;
    size_t n = (size_t)digits;
    if (!v && !c->precision) n=0;
    size_t pn=0;
    if (sign_conversion || pointer) {
        if (negative) prefix[pn++]='-';
        else if (c->flags&F_PLUS) prefix[pn++]='+';
        else if (c->flags&F_SPACE) prefix[pn++]=' ';
    }
    if (pointer || (base==16 && v && (c->flags&F_ALT))) {
        prefix[pn++]='0';
        prefix[pn++]=c->conversion=='X' ? 'X' : 'x';
    }
    size_t zeroes=c->precision>0 && (size_t)c->precision>n ? (size_t)c->precision-n : 0;
    if (base==8 && (c->flags&F_ALT) && (!n || (v && !zeroes))) ++zeroes;
    conversion style=*c;
    if (c->precision>=0) style.flags&=~F_ZERO;
    return field(o, &style, prefix, pn, buf, n, n, zeroes, 1);
}

static int string(output *o, const conversion *c, const argument *a, int wide)
{
    const void *ptr=(const void *)(uintptr_t)word(a);
    if (!ptr) {
        size_t n=c->precision>=0 && c->precision<6 ? 0 : 6;
        return field(o, c, "", 0, "(null)", n, n, 0, 0);
    }
    size_t bytes=0, units=0;
    if (wide) {
        const uint32_t *s=ptr;
        char encoded[4];
        while (*s) {
            if (o->wide && c->precision>=0 && units>=(size_t)c->precision) break;
            int n=lx_utf8_encode(*s, encoded);
            if (n<0) return lx_set_errno(LX_EILSEQ);
            if (!o->wide && c->precision>=0 && (size_t)n>(size_t)c->precision-bytes) break;
            bytes+=(size_t)n;
            ++units;
            ++s;
        }
        size_t length=o->wide ? units : bytes;
        size_t pad=c->width>0 && (size_t)c->width>length ? (size_t)c->width-length : 0;
        if (!(c->flags&F_LEFT) && repeat(o, ' ', pad)<0) return -1;
        s=ptr;
        for (size_t i=0; i<units; ++i) {
            int n=lx_utf8_encode(s[i], encoded);
            if (emit(o, encoded, (size_t)n)<0) return -1;
        }
        if ((c->flags&F_LEFT) && repeat(o, ' ', pad)<0) return -1;
        return 0;
    }
    const char *s=ptr;
    if (o->wide) {
        while (s[bytes] && (c->precision<0 || units<(size_t)c->precision)) {
            uint32_t wc;
            size_t available=0;
            while (available<4 && s[bytes+available]) ++available;
            int n=lx_utf8_decode((const unsigned char *)s+bytes, available, &wc);
            if (n<=0) return lx_set_errno(LX_EILSEQ);
            bytes+=(size_t)n;
            ++units;
        }
    } else {
        while (s[bytes] && (c->precision<0 || bytes<(size_t)c->precision)) ++bytes;
        units=bytes;
    }
    return field(o, c, "", 0, s, bytes, units, 0, 0);
}

/* Match the guest x87 rounding control, including directed rounding of
 * negative values. comparison is the discarded fraction versus one half. */
static int round_increment(int negative, int comparison, int odd, int discarded)
{
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    switch ((control>>10)&3) {
    case 1: return negative && discarded;
    case 2: return !negative && discarded;
    case 3: return 0;
    default: return comparison>0 || (!comparison && odd);
    }
}

/* x87's explicit integer bit gives %La a different leading hexadecimal digit
 * from %a. Keep the original significand, including subnormal low bits. */
static int hexadecimal(output *o, const conversion *c, uint64_t significand,
                       int fraction_bits, int exponent, int negative)
{
    int upper=c->conversion=='A';
    const char *digits=upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int full=fraction_bits/4;
    int precision=c->precision;
    if (precision<0) {
        precision=full;
        uint64_t tail=significand;
        while (precision && !(tail&15)) { --precision; tail>>=4; }
    }
    int stored=precision<full ? precision : full;
    uint64_t rounded=significand;
    if (stored<full) {
        unsigned shift=(unsigned)(fraction_bits-4*stored);
        uint64_t remainder=significand&((((uint64_t)1)<<shift)-1);
        uint64_t half=((uint64_t)1)<<(shift-1);
        rounded=significand>>shift;
        int comparison=remainder>half ? 1 : remainder==half ? 0 : -1;
        if (round_increment(negative, comparison, (int)(rounded&1), remainder!=0)) ++rounded;
        if ((rounded>>(4*stored))>=16) { rounded>>=4; exponent+=4; }
    }
    char body[64], prefix[3];
    size_t n=0, pn=0;
    if (negative) prefix[pn++]='-';
    else if (c->flags&F_PLUS) prefix[pn++]='+';
    else if (c->flags&F_SPACE) prefix[pn++]=' ';
    prefix[pn++]='0'; prefix[pn++]=upper ? 'X' : 'x';
    body[n++]=digits[(unsigned)(rounded>>(4*stored))&15];
    if (precision || (c->flags&F_ALT)) body[n++]='.';
    for (int i=stored-1; i>=0; --i) body[n++]=digits[(unsigned)(rounded>>(4*i))&15];
    char expbuf[16];
    unsigned magnitude=(unsigned)(exponent<0 ? -exponent : exponent);
    size_t en=unsigned_digits(expbuf+sizeof expbuf, magnitude, 10, 0);
    size_t trailing=(size_t)(precision-stored);
    size_t total=n+trailing+2+en;
    size_t width=c->width>0 && (size_t)c->width>pn+total ? (size_t)c->width-pn-total : 0;
    if (!(c->flags&(F_LEFT|F_ZERO)) && repeat(o, ' ', width)<0) return -1;
    if (emit(o, prefix, pn)<0) return -1;
    if ((c->flags&F_ZERO) && !(c->flags&F_LEFT) && repeat(o, '0', width)<0) return -1;
    if (emit(o, body, n)<0 || repeat(o, '0', trailing)<0) return -1;
    char marker[2]={upper ? 'P':'p', exponent<0 ? '-':'+'};
    if (emit(o, marker, 2)<0 || emit(o, expbuf+sizeof expbuf-en, en)<0) return -1;
    return (c->flags&F_LEFT) ? repeat(o, ' ', width) : 0;
}

/* Exact x87 decimal expansion, base 10^9. A finite binary value has a finite
 * decimal expansion; round it once, not first to binary64 and then to decimal. */
static char *extended_digits(uint64_t mantissa, unsigned exponent, size_t *length,
                             int *power, char *local, size_t local_size)
{
    int binary_power=(exponent ? (int)exponent-16383 : -16382)-63;
    if (!mantissa) {
        local[0]='0'; local[1]=0; *length=1; *power=0;
        return local;
    }
    while (!(mantissa&1)) { mantissa>>=1; ++binary_power; }
    unsigned iterations=(unsigned)(binary_power<0 ? -binary_power : binary_power);
    unsigned multiplier=binary_power<0 ? 5 : 2;
    size_t decimal_bound=21+((size_t)iterations*(multiplier==5 ? 699 : 302)+999)/1000;
    size_t capacity=decimal_bound/9+1;
    uint32_t local_limbs[64];
    uint32_t *limbs=capacity<=LX_COUNT(local_limbs) ? local_limbs : lx_malloc(capacity*sizeof *limbs);
    if (!limbs) { lx_set_errno(LX_ENOMEM); return NULL; }
    size_t used=0;
    do { limbs[used++]=(uint32_t)(mantissa%1000000000); mantissa/=1000000000; } while (mantissa);
    for (unsigned i=0; i<iterations; ++i) {
        uint64_t carry=0;
        for (size_t j=0; j<used; ++j) {
            uint64_t v=(uint64_t)limbs[j]*multiplier+carry;
            limbs[j]=(uint32_t)(v%1000000000);
            carry=v/1000000000;
        }
        if (carry) limbs[used++]=(uint32_t)carry;
    }
    char *s=used*9+1<=local_size ? local : lx_malloc(used*9+1);
    if (!s) {
        if (limbs!=local_limbs) lx_free(limbs);
        lx_set_errno(LX_ENOMEM);
        return NULL;
    }
    char top[10];
    size_t n=unsigned_digits(top+sizeof top, limbs[used-1], 10, 0);
    memcpy(s, top+sizeof top-n, n);
    for (size_t i=used-1; i; --i) {
        uint32_t v=limbs[i-1];
        for (int j=8; j>=0; --j) { s[n+(size_t)j]=(char)('0'+v%10); v/=10; }
        n+=9;
    }
    if (limbs!=local_limbs) lx_free(limbs);
    s[n]=0;
    *length=n;
    *power=(int)n-1+(binary_power<0 ? binary_power : 0);
    return s;
}
static void round_decimal(char *s, size_t *length, int64_t keep, int *power, int negative)
{
    size_t n=*length;
    if (keep>=(int64_t)n) return;
    int comparison=-1, discarded=0, odd=0;
    size_t k=keep<0 ? 0 : (size_t)keep;
    for (size_t i=k; i<n; ++i) if (s[i]!='0') { discarded=1; break; }
    if (keep>=0) {
        comparison=s[k]>'5' ? 1 : s[k]<'5' ? -1 : 0;
        if (!comparison)
            for (size_t i=k+1; i<n; ++i) if (s[i]!='0') { comparison=1; break; }
        odd=k && ((s[k-1]-'0')&1);
    }
    int up=round_increment(negative, comparison, odd, discarded);
    if (keep<=0) {
        s[0]=up ? '1' : '0';
        *length=1;
        *power=up ? (int)((int64_t)*power+1-keep) : 0;
        return;
    }
    n=(size_t)keep;
    if (up) {
        size_t i=n;
        while (i && s[i-1]=='9') s[--i]='0';
        if (i) ++s[i-1];
        else { s[0]='1'; ++*power; }
    }
    *length=n;
}
static int extended_decimal(output *o, const conversion *c, uint64_t mantissa, unsigned exponent, int negative)
{
    size_t n;
    int power;
    char local[256];
    char *digits=extended_digits(mantissa, exponent, &n, &power, local, sizeof local);
    if (!digits) return -1;
    int lower=c->conversion|32;
    int precision=c->precision<0 ? 6 : c->precision;
    if (lower=='g' && !precision) precision=1;
    int64_t keep=lower=='f' ? (int64_t)power+1+precision : lower=='e' ? (int64_t)precision+1 : precision;
    round_decimal(digits, &n, keep, &power, negative);
    int scientific=lower=='e' || (lower=='g' && (power<-4 || power>=precision));
    size_t fractional;
    if (lower=='g') {
        if (!(c->flags&F_ALT)) while (n>1 && digits[n-1]=='0') --n;
        if (scientific) fractional=(c->flags&F_ALT) ? (size_t)precision-1 : n-1;
        else {
            int64_t f=(c->flags&F_ALT) ? (int64_t)precision-power-1 : (int64_t)n-power-1;
            fractional=f>0 ? (size_t)f : 0;
        }
    } else fractional=(size_t)precision;
    size_t integer_count=scientific || power<0 ? 1 : (size_t)power+1;
    char expbuf[16];
    size_t en=0;
    if (scientific) {
        unsigned magnitude=(unsigned)(power<0 ? -power : power);
        en=unsigned_digits(expbuf+sizeof expbuf, magnitude, 10, 0);
    }
    size_t total=integer_count+fractional+(fractional || (c->flags&F_ALT) ? 1 : 0)+(scientific ? 2+(en<2 ? 2:en) : 0);
    if (total>INT_MAX) {
        if (digits!=local) lx_free(digits);
        return lx_set_errno(LX_EOVERFLOW);
    }
    char prefix[1];
    size_t pn=0;
    if (negative) prefix[pn++]='-';
    else if (c->flags&F_PLUS) prefix[pn++]='+';
    else if (c->flags&F_SPACE) prefix[pn++]=' ';
    size_t pad=c->width>0 && (size_t)c->width>total+pn ? (size_t)c->width-total-pn : 0;
    int result=-1;
    if (!(c->flags&(F_ZERO|F_LEFT)) && repeat(o, ' ', pad)<0) goto done;
    if (emit(o, prefix, pn)<0) goto done;
    if ((c->flags&F_ZERO) && !(c->flags&F_LEFT) && repeat(o, '0', pad)<0) goto done;
    if (scientific) {
        if (emit(o, digits, 1)<0) goto done;
    } else if (power<0) {
        if (emit(o, "0", 1)<0) goto done;
    } else {
        size_t copied=integer_count<n ? integer_count:n;
        if (emit(o, digits, copied)<0 || repeat(o, '0', integer_count-copied)<0) goto done;
    }
    if ((fractional || (c->flags&F_ALT)) && emit(o, ".", 1)<0) goto done;
    if (fractional) {
        int start=scientific ? 1 : power+1;
        size_t leading=start<0 ? (size_t)-start : 0;
        if (leading>fractional) leading=fractional;
        if (repeat(o, '0', leading)<0) goto done;
        size_t remaining=fractional-leading;
        size_t index=start<0 ? 0 : (size_t)start;
        size_t copied=index<n ? n-index : 0;
        if (copied>remaining) copied=remaining;
        if (emit(o, copied ? digits+index : "", copied)<0 || repeat(o, '0', remaining-copied)<0) goto done;
    }
    if (scientific) {
        char marker[2]={c->conversion=='E' || c->conversion=='G' ? 'E':'e', power<0 ? '-':'+'};
        if (emit(o, marker, 2)<0 || (en<2 && emit(o, "0", 1)<0) || emit(o, expbuf+sizeof expbuf-en, en)<0) goto done;
    }
    if ((c->flags&F_LEFT) && repeat(o, ' ', pad)<0) goto done;
    result=0;
done:
    if (digits!=local) lx_free(digits);
    return result;
}

static int floating(output *o, const conversion *c, const argument *a)
{
    double value;
    uint64_t bits, mantissa;
    unsigned exponent;
    int negative, special;
    int extended=c->length==LEN_CAPL;
    if (extended) {
        uint16_t se;
        mantissa=quad(a);
        memcpy(&se, a->p+8, sizeof se);
        exponent=se&0x7fff;
        negative=se>>15;
        special=exponent==0x7fff ? (mantissa==UINT64_C(0x8000000000000000) ? 1 : 2) : 0;
        /* Guest long double occupies twelve stack bytes, not Windows' eight. */
        __asm__ volatile("fldt %1\n\tfstpl %0" : "=m"(value) : "m"(*(const unsigned char (*)[12])a->p) : "st");
    } else {
        bits=quad(a);
        memcpy(&value, &bits, sizeof value);
        negative=(int)(bits>>63);
        exponent=(unsigned)((bits>>52)&0x7ff);
        mantissa=bits&UINT64_C(0xfffffffffffff);
        special=exponent==0x7ff ? (mantissa ? 2 : 1) : 0;
    }
    if (special) {
        int upper=c->conversion>='A' && c->conversion<='Z';
        const char *body=special==1 ? (upper ? "INF":"inf") : (upper ? "NAN":"nan");
        char sign=negative ? '-' : (c->flags&F_PLUS) ? '+' : ' ';
        size_t pn=negative || (c->flags&(F_PLUS|F_SPACE)) ? 1 : 0;
        return field(o, c, &sign, pn, body, 3, 3, 0, 0);
    }
    if (c->conversion=='a' || c->conversion=='A') {
        if (extended) return hexadecimal(o, c, mantissa, 60, mantissa ? (exponent ? (int)exponent-16383 : -16382)-3 : 0, negative);
        uint64_t sig=mantissa|(exponent ? UINT64_C(1)<<52 : 0);
        return hexadecimal(o, c, sig, 52, sig ? (exponent ? (int)exponent-1023 : -1022) : 0, negative);
    }
    if (extended) return extended_decimal(o, c, mantissa, exponent, negative);
    char spec[8];
    char *p=spec;
    *p++='%';
    if (c->flags&F_ALT) *p++='#';
    if (c->precision>=0) { *p++='.'; *p++='*'; }
    *p++=(char)c->conversion;
    *p=0;
    char local[512];
    int n=c->precision>=0 ? snprintf(local, sizeof local, spec, c->precision, value) : snprintf(local, sizeof local, spec, value);
    if (n<0) return lx_set_errno(LX_EOVERFLOW);
    char *body=local;
    if ((size_t)n>=sizeof local) {
        body=lx_malloc((size_t)n+1);
        if (!body) return lx_set_errno(LX_ENOMEM);
        if (c->precision>=0) snprintf(body, (size_t)n+1, spec, c->precision, value);
        else snprintf(body, (size_t)n+1, spec, value);
    }
    char sign=negative ? '-' : (c->flags&F_PLUS) ? '+' : ' ';
    size_t pn=negative || (c->flags&(F_PLUS|F_SPACE)) ? 1 : 0;
    size_t skip=negative && body[0]=='-' ? 1 : 0;
    size_t length=(size_t)n-skip;
    int result=field(o, c, &sign, pn, body+skip, length, length, 0, 1);
    if (body!=local) lx_free(body);
    return result;
}

int lx_vformat(lx_format_sink sink, void *ctx, const char *fmt, va_list ap)
{
    int saved_errno=lx_errno;
    argument local[32]={0}, *args=local;
    size_t capacity=LX_COUNT(local);
    int maximum=0, next=1, result=-1;
    const char *p=fmt;
    if (!fmt) return lx_set_errno(LX_EINVAL);
    while (*p) {
        if (*p++!='%') continue;
        conversion c;
        if (parse(p, &next, &c)<0) goto done;
        if (register_argument(&args, &capacity, local, c.width_index, ARG_WORD)<0 ||
            register_argument(&args, &capacity, local, c.precision_index, ARG_WORD)<0 ||
            register_argument(&args, &capacity, local, c.index, argument_type(&c))<0) goto done;
        if (c.index>maximum) maximum=c.index;
        if (c.width_index>maximum) maximum=c.width_index;
        if (c.precision_index>maximum) maximum=c.precision_index;
        p=c.end;
    }
    const unsigned char *cursor=(const unsigned char *)ap;
    for (int i=1; i<=maximum; ++i) {
        args[i].p=cursor;
        cursor+=args[i].type==ARG_EXTENDED ? 12 : (args[i].type==ARG_QUAD || args[i].type==ARG_DOUBLE) ? 8 : 4;
    }
    output out={sink, ctx, 0, sink==wide_sink};
    p=fmt;
    next=1;
    while (*p) {
        const char *literal=p;
        while (*p && *p!='%') ++p;
        if (emit(&out, literal, (size_t)(p-literal))<0) goto done;
        if (!*p) break;
        const char *start=p++;
        conversion c;
        if (parse(p, &next, &c)<0) goto done;
        p=c.end;
        if (c.width_index) {
            c.width=(int32_t)word(&args[c.width_index]);
            if (c.width<0) {
                if (c.width==INT_MIN) { lx_set_errno(LX_EOVERFLOW); goto done; }
                c.width=-c.width;
                c.flags|=F_LEFT;
            }
        }
        if (c.precision_index) {
            c.precision=(int32_t)word(&args[c.precision_index]);
            if (c.precision<0) c.precision=-1;
        }
        const argument *a=&args[c.index];
        if (strchr("diuoxXp", c.conversion)) {
            if (integer(&out, &c, a)<0) goto done;
        } else if (strchr("eEfFgGaA", c.conversion)) {
            if (floating(&out, &c, a)<0) goto done;
        } else if (c.conversion=='s' || c.conversion=='S') {
            if (string(&out, &c, a, c.length==LEN_L || c.conversion=='S')<0) goto done;
        } else if (c.conversion=='c' || c.conversion=='C') {
            char buf[4];
            int wide=c.length==LEN_L || c.conversion=='C';
            uint32_t wc=word(a);
            int n=1;
            if (wide || out.wide) {
                if (!wide) wc=(unsigned char)wc;
                n=lx_utf8_encode(wc, buf);
                if (n<0) { lx_set_errno(LX_EILSEQ); goto done; }
            } else buf[0]=(char)wc;
            if (field(&out, &c, "", 0, buf, (size_t)n, out.wide ? 1 : (size_t)n, 0, 0)<0) goto done;
        } else if (c.conversion=='n') {
            void *dest=(void *)(uintptr_t)word(a);
            switch (c.length) {
            case LEN_HH: *(signed char *)dest=(signed char)out.count; break;
            case LEN_H: *(short *)dest=(short)out.count; break;
            case LEN_LL: case LEN_CAPL: case LEN_J: *(int64_t *)dest=out.count; break;
            default: *(int32_t *)dest=out.count; break;
            }
        } else if (c.conversion=='m') {
            const char *message=lx_strerror(saved_errno);
            size_t n=strlen(message);
            if (c.precision>=0 && (size_t)c.precision<n) n=(size_t)c.precision;
            if (field(&out, &c, "", 0, message, n, n, 0, 0)<0) goto done;
        } else if (c.conversion=='%') {
            if (emit(&out, "%", 1)<0) goto done;
        } else if (emit(&out, start, (size_t)(p-start))<0) goto done;
    }
    result=out.count;
done:
    if (args!=local) lx_free(args);
    return result;
}

/* Sinks return zero on success, -1 on failure, never a short positive count. */
typedef struct { char *s; size_t capacity, position; } buffer;
static int buffer_sink(void *ctx, const char *s, size_t n)
{
    buffer *b=ctx;
    size_t available=b->capacity && b->position<b->capacity-1 ? b->capacity-1-b->position : 0;
    size_t copied=n<available ? n : available;
    if (copied) memcpy(b->s+b->position, s, copied);
    b->position+=copied;
    return 0;
}
static int vsnprintf_(char *s, size_t n, const char *fmt, va_list ap)
{
    buffer b={s,n,0};
    int result=lx_vformat(buffer_sink, &b, fmt, ap);
    if (n) s[b.position]=0;
    return result;
}
static int snprintf_(char *s, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vsnprintf_(s,n,fmt,ap);
    va_end(ap); return result;
}
static int vsprintf_(char *s, const char *fmt, va_list ap)
{
    return vsnprintf_(s,SIZE_MAX,fmt,ap);
}
static int sprintf_(char *s, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vsprintf_(s,fmt,ap);
    va_end(ap); return result;
}
static int file_sink(void *ctx, const char *s, size_t n)
{
    return lx_fwrite_unlocked(s,1,n,ctx)==n ? 0 : -1;
}
static int vfprintf_(lx_FILE *f, const char *fmt, va_list ap)
{
    lx_flockfile(f);
    int result=lx_vformat(file_sink,f,fmt,ap);
    lx_funlockfile(f);
    return result;
}
static int fprintf_(lx_FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vfprintf_(f,fmt,ap);
    va_end(ap); return result;
}
static int vprintf_(const char *fmt, va_list ap) { return vfprintf_(lx_stdout,fmt,ap); }
static int printf_(const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vprintf_(fmt,ap);
    va_end(ap); return result;
}
static int fd_sink(void *ctx, const char *s, size_t n)
{
    int fd=(int)(intptr_t)ctx;
    while (n) {
        long z=lx_write(fd,s,(unsigned long)n);
        if (z<0) return -1;
        if (!z) return lx_set_errno(LX_EIO);
        s+=z; n-=(size_t)z;
    }
    return 0;
}
static int vdprintf_(int fd, const char *fmt, va_list ap)
{
    return lx_vformat(fd_sink,(void *)(intptr_t)fd,fmt,ap);
}
static int dprintf_(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vdprintf_(fd,fmt,ap);
    va_end(ap); return result;
}
typedef struct { char *s; size_t capacity, length; } allocated_buffer;
static int allocated_sink(void *ctx, const char *s, size_t n)
{
    allocated_buffer *b=ctx;
    if (n>SIZE_MAX-b->length-1) return lx_set_errno(LX_EOVERFLOW);
    size_t needed=b->length+n+1;
    if (needed>b->capacity) {
        size_t capacity=b->capacity ? b->capacity : 128;
        while (capacity<needed) {
            if (capacity>SIZE_MAX/2) { capacity=needed; break; }
            capacity*=2;
        }
        char *p=lx_realloc(b->s,capacity);
        if (!p) return lx_set_errno(LX_ENOMEM);
        b->s=p; b->capacity=capacity;
    }
    memcpy(b->s+b->length,s,n);
    b->length+=n;
    return 0;
}
static int vasprintf_(char **s, const char *fmt, va_list ap)
{
    allocated_buffer b={0};
    *s=NULL;
    int result=lx_vformat(allocated_sink,&b,fmt,ap);
    if (result<0) { lx_free(b.s); return -1; }
    if (!b.s) {
        b.s=lx_malloc(1);
        if (!b.s) return lx_set_errno(LX_ENOMEM);
    }
    b.s[b.length]=0;
    *s=b.s;
    return result;
}
static int asprintf_(char **s, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vasprintf_(s,fmt,ap);
    va_end(ap); return result;
}

typedef struct { uint32_t *s; size_t capacity, position; int overflow; } wide_buffer;
static int wide_sink(void *ctx, const char *s, size_t n)
{
    wide_buffer *b=ctx;
    for (size_t i=0; i<n;) {
        uint32_t wc;
        int z=lx_utf8_decode((const unsigned char *)s+i,n-i,&wc);
        if (z<=0) return lx_set_errno(LX_EILSEQ);
        if (b->position>=b->capacity) b->overflow=1;
        else b->s[b->position++]=wc;
        i+=(size_t)z;
    }
    return 0;
}
static int vswprintf_(uint32_t *s, size_t n, const uint32_t *fmt, va_list ap)
{
    if (!n) return -1;
    s[0]=0;
    size_t length=0;
    for (const uint32_t *p=fmt; *p; ++p) {
        char encoded[4];
        int z=lx_utf8_encode(*p,encoded);
        if (z<0) return lx_set_errno(LX_EILSEQ);
        if (length>SIZE_MAX-(size_t)z-1) return lx_set_errno(LX_ENOMEM);
        length+=(size_t)z;
    }
    char local[256], *format=length<sizeof local ? local : lx_malloc(length+1);
    if (!format) return lx_set_errno(LX_ENOMEM);
    size_t at=0;
    for (const uint32_t *p=fmt; *p; ++p) {
        int z=lx_utf8_encode(*p,format+at);
        at+=(size_t)z;
    }
    format[at]=0;
    wide_buffer b={s,n-1,0,0};
    int result=lx_vformat(wide_sink,&b,format,ap);
    /* glibc reserves one slot, but does not terminate a truncated result.
     * Continue formatting after truncation so %n still observes the count. */
    if (!b.overflow) s[b.position]=0;
    else result=-1;
    if (format!=local) lx_free(format);
    return result;
}
static int swprintf_(uint32_t *s, size_t n, const uint32_t *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vswprintf_(s,n,fmt,ap);
    va_end(ap); return result;
}

/* glibc fortify ABI: the flag follows the stream/fd, or follows snprintf's
 * explicit bound. Object sizes include the terminating NUL. */
static int vsprintf_chk_(char *s, int flag, size_t size, const char *fmt, va_list ap)
{
    (void)flag;
    int result=vsnprintf_(s,size,fmt,ap);
    if (result>=0 && (size_t)result>=size) lx_fatal("*** buffer overflow detected ***: terminated");
    return result;
}
static int sprintf_chk_(char *s, int flag, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vsprintf_chk_(s,flag,size,fmt,ap);
    va_end(ap); return result;
}
static int vsnprintf_chk_(char *s, size_t n, int flag, size_t size, const char *fmt, va_list ap)
{
    (void)flag;
    if (n>size) lx_fatal("*** buffer overflow detected ***: terminated");
    return vsnprintf_(s,n,fmt,ap);
}
static int snprintf_chk_(char *s, size_t n, int flag, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vsnprintf_chk_(s,n,flag,size,fmt,ap);
    va_end(ap); return result;
}
static int vfprintf_chk_(lx_FILE *f, int flag, const char *fmt, va_list ap)
{ (void)flag; return vfprintf_(f,fmt,ap); }
static int fprintf_chk_(lx_FILE *f, int flag, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vfprintf_chk_(f,flag,fmt,ap);
    va_end(ap); return result;
}
static int vprintf_chk_(int flag, const char *fmt, va_list ap)
{ (void)flag; return vprintf_(fmt,ap); }
static int printf_chk_(int flag, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vprintf_chk_(flag,fmt,ap);
    va_end(ap); return result;
}
static int vasprintf_chk_(char **s, int flag, const char *fmt, va_list ap)
{ (void)flag; return vasprintf_(s,fmt,ap); }
static int asprintf_chk_(char **s, int flag, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    int result=vasprintf_chk_(s,flag,fmt,ap);
    va_end(ap); return result;
}
static int dprintf_chk_(int fd, int flag, const char *fmt, ...)
{
    (void)flag;
    va_list ap; va_start(ap,fmt);
    int result=vdprintf_(fd,fmt,ap);
    va_end(ap); return result;
}

LX_SYMTAB(format,
    LX_SYM("printf",printf_), LX_SYM("fprintf",fprintf_),
    LX_SYM("sprintf",sprintf_), LX_SYM("snprintf",snprintf_),
    LX_SYM("vprintf",vprintf_), LX_SYM("vfprintf",vfprintf_),
    LX_SYM("vsprintf",vsprintf_), LX_SYM("vsnprintf",vsnprintf_),
    LX_SYM("dprintf",dprintf_), LX_SYM("vdprintf",vdprintf_),
    LX_SYM("asprintf",asprintf_), LX_SYM("vasprintf",vasprintf_),
    LX_SYM("swprintf",swprintf_), LX_SYM("vswprintf",vswprintf_),
    LX_SYM("__printf_chk",printf_chk_), LX_SYM("__fprintf_chk",fprintf_chk_),
    LX_SYM("__sprintf_chk",sprintf_chk_), LX_SYM("__snprintf_chk",snprintf_chk_),
    LX_SYM("__vsprintf_chk",vsprintf_chk_), LX_SYM("__vsnprintf_chk",vsnprintf_chk_),
    LX_SYM("__vfprintf_chk",vfprintf_chk_), LX_SYM("__vprintf_chk",vprintf_chk_),
    LX_SYM("__asprintf_chk",asprintf_chk_), LX_SYM("__vasprintf_chk",vasprintf_chk_),
    LX_SYM("__dprintf_chk",dprintf_chk_)
);
