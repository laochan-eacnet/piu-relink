/* glibc i386 iconv ABI. The conversion cursor is committed one whole character
 * at a time: E2BIG, EILSEQ and EINVAL leave the offending input unconsumed.
 * UTF-16/UTF-32 have a descriptor-local BOM; explicit-endian and UCS encodings
 * never add or remove one. Windows is only the legacy mapping-table provider,
 * not the owner of guest buffers, errno, replacement policy or wchar_t ABI.
 * Empty encoding names follow LC_CTYPE. UCS-4/UTF-8 preserve glibc's historical
 * 31-bit repertoire; UTF-16/32 reject non-scalars. //IGNORE consumes errors but
 * still returns EILSEQ. //TRANSLIT uses '?' in C and Windows Unicode folding
 * in C.UTF-8 (not the complete glibc locale transliteration tables).
 * Reset re-arms BOM detection/emission; glibc's detected endian swap is sticky
 * across resets. PIU itself requests UTF-16 and UTF-32. */
#include "lxlocale.h"

#include <string.h>

enum encoding_kind {
    ENC_UTF8, ENC_ASCII, ENC_LATIN1, ENC_UTF16, ENC_UCS2, ENC_UCS4,
    ENC_UTF32, ENC_WCHAR, ENC_CP949, ENC_EUCKR, ENC_SJIS, ENC_GBK, ENC_CP1252
};
typedef struct {
    enum encoding_kind kind;
    unsigned be, bom;
} encoding;
typedef struct {
    encoding from, to;
    unsigned ignore, translit, input_started, output_started, input_be;
} iconv_descriptor;

static size_t iconv_error(int error)
{
    lx_errno = error;
    return (size_t)-1;
}

static unsigned ascii_upper(unsigned c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static int option_equal(const char *s, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n && name[i]; ++i)
        if (ascii_upper((unsigned char)s[i]) != (unsigned char)name[i])
            return 0;
    return i == n && !name[i];
}

static int parse_encoding(const char *name, encoding *e, unsigned *ignore, unsigned *translit)
{
    char key[64];
    size_t n = 0;
    const char *suffix;
    if (!name)
        return 0;
    suffix = strstr(name, "//");
    for (const char *p = name; *p && p != suffix; ++p) {
        unsigned c = (unsigned char)*p;
        if (c == '-' || c == '_' || c == ' ' || c == '.' || c == ':')
            continue;
        if (n + 1 >= sizeof key)
            return 0;
        key[n++] = (char)ascii_upper(c);
    }
    key[n] = 0;
    if (suffix && ignore && translit) {
        const char *p = suffix + 2;
        while (*p) {
            const char *start = p;
            while (*p && *p != ',' && *p != '/')
                ++p;
            if (option_equal(start, (size_t)(p - start), "IGNORE"))
                *ignore = 1;
            if (option_equal(start, (size_t)(p - start), "TRANSLIT"))
                *translit = 1;
            while (*p == ',' || *p == '/')
                ++p;
        }
    }
    e->be = e->bom = 0;
    if (!n) {
        e->kind = lx_locale_utf8(NULL) ? ENC_UTF8 : ENC_ASCII;
    } else if (!strcmp(key, "UTF8") || !strcmp(key, "ISO10646/UTF8")) {
        e->kind = ENC_UTF8;
    } else if (!strcmp(key, "ASCII") || !strcmp(key, "USASCII") ||
               !strcmp(key, "ANSIX341968") || !strcmp(key, "ANSIX341986") ||
               !strcmp(key, "ISO646US") || !strcmp(key, "ISOIR6") ||
               !strcmp(key, "ISO646IRV1991") || !strcmp(key, "CP367") ||
               !strcmp(key, "IBM367") || !strcmp(key, "CSASCII")) {
        e->kind = ENC_ASCII;
    } else if (!strcmp(key, "ISO88591") || !strcmp(key, "ISO885911987") ||
               !strcmp(key, "LATIN1") || !strcmp(key, "L1") ||
               !strcmp(key, "CP819") || !strcmp(key, "IBM819") ||
               !strcmp(key, "CSISOLATIN1")) {
        e->kind = ENC_LATIN1;
    } else if (!strcmp(key, "UTF16") || !strcmp(key, "UTF16LE") || !strcmp(key, "UTF16BE")) {
        e->kind = ENC_UTF16;
        e->bom = !strcmp(key, "UTF16");
        e->be = !strcmp(key, "UTF16BE");
    } else if (!strcmp(key, "UCS2") || !strcmp(key, "UCS2LE") ||
               !strcmp(key, "UCS2BE") || !strcmp(key, "UCS2INTERNAL") ||
               !strcmp(key, "UNICODELITTLE") || !strcmp(key, "UNICODEBIG") ||
               !strcmp(key, "ISO10646/UCS2")) {
        /* glibc UCS-2 is native endian, unlike its big-endian UCS-4. */
        e->kind = ENC_UCS2;
        e->be = !strcmp(key, "UCS2BE") || !strcmp(key, "UNICODEBIG");
    } else if (!strcmp(key, "UCS4") || !strcmp(key, "UCS4LE") ||
               !strcmp(key, "UCS4BE") || !strcmp(key, "UCS4INTERNAL") ||
               !strcmp(key, "ISO10646/UCS4")) {
        e->kind = ENC_UCS4;
        e->be = strcmp(key, "UCS4LE") && strcmp(key, "UCS4INTERNAL");
    } else if (!strcmp(key, "UTF32") || !strcmp(key, "UTF32LE") || !strcmp(key, "UTF32BE")) {
        e->kind = ENC_UTF32;
        e->bom = !strcmp(key, "UTF32");
        e->be = !strcmp(key, "UTF32BE");
    } else if (!strcmp(key, "WCHART")) {
        e->kind = ENC_WCHAR; /* Linux i386 wchar_t is UCS-4LE, never Win32 UTF-16. */
    } else if (!strcmp(key, "CP949") || !strcmp(key, "MS949") ||
               !strcmp(key, "WINDOWS949") || !strcmp(key, "UHC")) {
        e->kind = ENC_CP949;
    } else if (!strcmp(key, "EUCKR") || !strcmp(key, "CSEUCKR") || !strcmp(key, "KSC5601")) {
        e->kind = ENC_EUCKR;
    } else if (!strcmp(key, "SHIFTJIS") || !strcmp(key, "SJIS") ||
               !strcmp(key, "CSSHIFTJIS") || !strcmp(key, "MSKANJI")) {
        e->kind = ENC_SJIS;
    } else if (!strcmp(key, "GBK") || !strcmp(key, "CP936") ||
               !strcmp(key, "MS936") || !strcmp(key, "WINDOWS936")) {
        e->kind = ENC_GBK;
    } else if (!strcmp(key, "CP1252") || !strcmp(key, "WINDOWS1252") || !strcmp(key, "MSANSI")) {
        e->kind = ENC_CP1252;
    } else {
        return 0;
    }
    return 1;
}

static unsigned get16(const unsigned char *p, unsigned be)
{
    return be ? ((unsigned)p[0] << 8) | p[1] : ((unsigned)p[1] << 8) | p[0];
}

static uint32_t get32(const unsigned char *p, unsigned be)
{
    if (be)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void put16(unsigned char *p, unsigned c, unsigned be)
{
    p[be ? 1 : 0] = (unsigned char)c;
    p[be ? 0 : 1] = (unsigned char)(c >> 8);
}

static void put32(unsigned char *p, uint32_t c, unsigned be)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[be ? 3 - i : i] = (unsigned char)c;
        c >>= 8;
    }
}

static unsigned codepage(enum encoding_kind kind)
{
    if (kind == ENC_CP949 || kind == ENC_EUCKR)
        return 949;
    if (kind == ENC_SJIS)
        return 932;
    return kind == ENC_GBK ? 936 : 1252;
}

/* Return a byte width, or zero for a bad lead/trail. Bad-width is the span
 * discarded by //IGNORE; a structurally valid but unmapped pair loses both. */
static int legacy_width(enum encoding_kind kind, const unsigned char *s, size_t n, size_t *bad)
{
    unsigned a = s[0], b;
    *bad = 1;
    if (a < 0x80 || (kind == ENC_EUCKR && a <= 0x9f))
        return 1;
    if (kind == ENC_CP1252)
        return a == 0x81 || a == 0x8d || a == 0x8f || a == 0x90 || a == 0x9d ? 0 : 1;
    if (kind == ENC_GBK && a == 0x80)
        return 1;
    if (kind == ENC_SJIS && a >= 0xa1 && a <= 0xdf)
        return 1;
    if (kind == ENC_EUCKR) {
        if (a < 0xa1 || a > 0xfe || a == 0xc9)
            return 0;
    } else if (kind == ENC_CP949) {
        if (a < 0x81 || a >= 0xfe || a == 0xc9)
            return 0;
    } else if (kind == ENC_SJIS) {
        if (a < 0x81 || a == 0xa0 || a > 0xea)
            return 0;
    } else if (a < 0x81 || a > 0xfe) {
        return 0;
    }
    if (n < 2)
        return -2;
    b = s[1];
    if (kind == ENC_EUCKR) {
        *bad = 2;
        if (b < 0xa1 || b > 0xfe)
            return 0;
    } else if (kind == ENC_CP949) {
        if (a < 0xa1 || b < 0xa1) {
            if (a > 0xc6 || b < 0x41 || b > 0xfe ||
                (b > 0x5a && b < 0x61) || (b > 0x7a && b < 0x81) ||
                (a == 0xc6 && b > 0x52))
                return 0;
        } else if (b > 0xfe) {
            return 0;
        }
    } else if (kind == ENC_SJIS) {
        unsigned pair = (a << 8) | b;
        if (b < 0x40)
            return 0;
        *bad = 2;
        if (b == 0x7f || b > 0xfc || (pair > 0x84be && pair < 0x889f) ||
            (pair > 0x88fc && pair < 0x8940) || (pair > 0x9ffc && pair < 0xe040) || pair > 0xeaa4)
            return 0;
    } else if (b < 0x40 || b > 0xfe || b == 0x7f) {
        return 0;
    }
    *bad = 2;
    return 2;
}

static uint32_t sjis_unicode(unsigned pair, uint32_t cp)
{
    /* JIS X 0208 differs from Windows-932 in these six assigned cells. */
    switch (pair) {
    case 0x8160: return 0x301c;
    case 0x8161: return 0x2016;
    case 0x817c: return 0x2212;
    case 0x8191: return 0x00a2;
    case 0x8192: return 0x00a3;
    case 0x81ca: return 0x00ac;
    default: return cp;
    }
}

static int decode_legacy(encoding e, const unsigned char *s, size_t n, uint32_t *cp, size_t *bad)
{
    wchar_t w[2];
    int width = legacy_width(e.kind, s, n, bad);
    if (width <= 0)
        return width == -2 ? -2 : -1;
    if (e.kind == ENC_SJIS && width == 1 && (s[0] == 0x5c || s[0] == 0x7e)) {
        *cp = s[0] == 0x5c ? 0xa5 : 0x203e;
        return 1;
    }
    if (e.kind == ENC_EUCKR && width == 1) {
        *cp = s[0]; /* glibc EUC-KR preserves the C1 control range. */
        return 1;
    }
    if (MultiByteToWideChar(codepage(e.kind), MB_ERR_INVALID_CHARS,
                            (const char *)s, width, w, 2) != 1 ||
        (w[0] >= 0xe000 && w[0] <= 0xf8ff))
        return -1;
    *cp = (uint16_t)w[0];
    if (e.kind == ENC_SJIS && width == 2)
        *cp = sjis_unicode(((unsigned)s[0] << 8) | s[1], *cp);
    if (e.kind == ENC_CP949 && *cp == 0x327e)
        return -1;
    return width;
}

/* glibc's UTF-8/UCS-4 codecs retain the historical 31-bit UCS repertoire.
 * UTF-16 and UTF-32 still enforce Unicode scalar values. */
static int decode_utf8(const unsigned char *s, size_t n, uint32_t *cp, size_t *bad)
{
    unsigned a = s[0], width;
    uint32_t c;
    if (a < 0x80) {
        *cp = a;
        return 1;
    }
    if (a >= 0xc2 && a < 0xe0) { width = 2; c = a & 0x1f; }
    else if (a >= 0xe0 && a < 0xf0) { width = 3; c = a & 0xf; }
    else if (a >= 0xf0 && a < 0xf8) { width = 4; c = a & 7; }
    else if (a >= 0xf8 && a < 0xfc) { width = 5; c = a & 3; }
    else if (a >= 0xfc && a < 0xfe) { width = 6; c = a & 1; }
    else {
        *bad = 1;
        while (*bad < n && *bad < 5 && (s[*bad] & 0xc0) == 0x80)
            ++*bad;
        return -1;
    }
    for (unsigned i = 1; i < width; ++i) {
        if (i == n)
            return -2;
        if ((s[i] & 0xc0) != 0x80) {
            *bad = i;
            return -1;
        }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *bad = width;
    if ((width > 2 && c < ((uint32_t)1 << (5 * width - 4))) ||
        (c >= 0xd800 && c <= 0xdfff))
        return -1;
    *cp = c;
    return (int)width;
}

static int decode(encoding e, unsigned be, const unsigned char *s, size_t n, uint32_t *cp, size_t *bad)
{
    uint32_t c;
    *bad = 1;
    if (e.kind == ENC_UTF8)
        return decode_utf8(s, n, cp, bad);
    if (e.kind == ENC_ASCII || e.kind == ENC_LATIN1) {
        if (e.kind == ENC_ASCII && s[0] >= 0x80)
            return -1;
        *cp = s[0];
        return 1;
    }
    if (e.kind >= ENC_CP949)
        return decode_legacy(e, s, n, cp, bad);
    if (e.kind == ENC_UTF16 || e.kind == ENC_UCS2) {
        if (n < 2)
            return -2;
        c = get16(s, be);
        *bad = 2;
        if (c >= 0xd800 && c <= 0xdfff) {
            unsigned low;
            if (e.kind == ENC_UCS2 || c >= 0xdc00)
                return -1;
            if (n < 4)
                return -2;
            low = get16(s + 2, be);
            if (low < 0xdc00 || low > 0xdfff)
                return -1;
            *cp = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            return 4;
        }
        *cp = c;
        return 2;
    }
    if (n < 4)
        return -2;
    c = get32(s, be);
    *bad = 4;
    if ((e.kind == ENC_UCS4 && c > 0x7fffffff) ||
        (e.kind == ENC_UTF32 && (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))))
        return -1;
    *cp = c;
    return 4;
}

static int encode_legacy(encoding e, uint32_t cp, unsigned char *s)
{
    wchar_t w;
    BOOL used_default = FALSE;
    size_t bad;
    int n;
    if (cp > 0xffff || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xe000 && cp <= 0xf8ff))
        return -1;
    if (e.kind == ENC_EUCKR) {
        if (cp <= 0x9f) {
            s[0] = (unsigned char)cp;
            return 1;
        }
        if (cp == 0x20a9) {
            s[0] = 0xa3;
            s[1] = 0xdc;
            return 2;
        }
    }
    if (e.kind == ENC_SJIS) {
        if (cp == 0xa5 || cp == 0x203e) {
            s[0] = cp == 0xa5 ? 0x5c : 0x7e;
            return 1;
        }
        switch (cp) {
        case 0x301c: s[0] = 0x81; s[1] = 0x60; return 2;
        case 0x2016: s[0] = 0x81; s[1] = 0x61; return 2;
        case 0x2212: s[0] = 0x81; s[1] = 0x7c; return 2;
        case 0x00a2: s[0] = 0x81; s[1] = 0x91; return 2;
        case 0x00a3: s[0] = 0x81; s[1] = 0x92; return 2;
        case 0x00ac: s[0] = 0x81; s[1] = 0xca; return 2;
        default: break;
        }
        if (cp == 0xff5e || cp == 0x2225 || cp == 0xff0d ||
            cp == 0xffe0 || cp == 0xffe1 || cp == 0xffe2)
            return -1;
    }
    if (e.kind == ENC_CP949 && cp == 0x327e)
        return -1;
    w = (wchar_t)cp;
    n = WideCharToMultiByte(codepage(e.kind), WC_NO_BEST_FIT_CHARS, &w, 1,
                            (char *)s, 4, NULL, &used_default);
    if (!n || used_default || legacy_width(e.kind, s, (size_t)n, &bad) != n)
        return -1;
    return n;
}

static int encode(encoding e, uint32_t cp, unsigned char *s)
{
    unsigned width;
    if (e.kind == ENC_ASCII || e.kind == ENC_LATIN1) {
        if (cp > (e.kind == ENC_ASCII ? 0x7fU : 0xffU))
            return -1;
        s[0] = (unsigned char)cp;
        return 1;
    }
    if (e.kind >= ENC_CP949)
        return encode_legacy(e, cp, s);
    if (e.kind == ENC_UCS4 || e.kind == ENC_WCHAR) {
        if (e.kind == ENC_UCS4 && cp > 0x7fffffff)
            return -1;
        put32(s, cp, e.be);
        return 4;
    }
    if (cp >= 0xd800 && cp <= 0xdfff)
        return -1;
    if (e.kind == ENC_UTF8) {
        if (cp > 0x7fffffff)
            return -1;
        if (cp < 0x80) {
            s[0] = (unsigned char)cp;
            return 1;
        }
        for (width = 2; width < 6; ++width)
            if (cp < ((uint32_t)1 << (5 * width + 1)))
                break;
        s[0] = (unsigned char)(0xffU << (8 - width));
        for (unsigned i = width - 1; i; --i) {
            s[i] = (unsigned char)(0x80 | (cp & 0x3f));
            cp >>= 6;
        }
        s[0] |= (unsigned char)cp;
        return (int)width;
    }
    if (cp > 0x10ffff || (e.kind == ENC_UCS2 && cp > 0xffff))
        return -1;
    if (e.kind == ENC_UTF32) {
        put32(s, cp, e.be);
        return 4;
    }
    if (cp < 0x10000) {
        put16(s, cp, e.be);
        return 2;
    }
    cp -= 0x10000;
    put16(s, 0xd800 + (cp >> 10), e.be);
    put16(s + 2, 0xdc00 + (cp & 0x3ff), e.be);
    return 4;
}

/* Transliteration is locale-dependent, as in glibc: C uses '?'; C.UTF-8
 * folds accents/ligatures and familiar typography, then uses '?'. The exact
 * non-ASCII transliteration repertoire follows Windows Unicode folding rather
 * than depending on host-installed glibc locale data. */
static int transliterate(encoding to, uint32_t cp, unsigned char *out)
{
    wchar_t original[1], expanded[16], folded[32];
    WORD types[32];
    const char *replacement = NULL;
    int count, n = 0;
    if (!lx_locale_utf8(NULL) || cp > 0xffff)
        return encode(to, '?', out);
    switch (cp) {
    case 0x00a0: replacement = " "; break;
    case 0x00a9: replacement = "(C)"; break;
    case 0x00ae: replacement = "(R)"; break;
    case 0x00c6: replacement = "AE"; break;
    case 0x00d0: replacement = "D"; break;
    case 0x00d8: replacement = "O"; break;
    case 0x00de: replacement = "TH"; break;
    case 0x00df: replacement = "ss"; break;
    case 0x00e6: replacement = "ae"; break;
    case 0x00f0: replacement = "d"; break;
    case 0x00f8: replacement = "o"; break;
    case 0x00fe: replacement = "th"; break;
    case 0x0110: replacement = "D"; break;
    case 0x0111: replacement = "d"; break;
    case 0x0141: replacement = "L"; break;
    case 0x0142: replacement = "l"; break;
    case 0x0152: replacement = "OE"; break;
    case 0x0153: replacement = "oe"; break;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: replacement = "-"; break;
    case 0x2014: replacement = "--"; break;
    case 0x2018: case 0x2019: case 0x201a: replacement = "'"; break;
    case 0x201c: case 0x201d: case 0x201e: replacement = "\""; break;
    case 0x2026: replacement = "..."; break;
    case 0x2122: replacement = "(TM)"; break;
    default: break;
    }
    if (replacement) {
        for (const char *p = replacement; *p; ++p) {
            int width = encode(to, (unsigned char)*p, out + n);
            if (width < 0)
                return encode(to, '?', out);
            n += width;
        }
        return n;
    }
    original[0] = (wchar_t)cp;
    count = FoldStringW(MAP_EXPAND_LIGATURES, original, 1, expanded, 16);
    if (!count)
        return encode(to, '?', out);
    count = FoldStringW(MAP_COMPOSITE | MAP_FOLDCZONE, expanded, count, folded, 32);
    if (!count || !GetStringTypeW(CT_CTYPE3, folded, count, types))
        return encode(to, '?', out);
    for (int i = 0; i < count; ++i) {
        int width;
        if (types[i] & (C3_NONSPACING | C3_DIACRITIC))
            continue;
        width = encode(to, (uint16_t)folded[i], out + n);
        if (width < 0)
            return encode(to, '?', out);
        n += width;
    }
    return n;
}

static void *iconv_open_(const char *tocode, const char *fromcode)
{
    iconv_descriptor initial = {0}, *cd;
    if (!parse_encoding(tocode, &initial.to, &initial.ignore, &initial.translit) ||
        !parse_encoding(fromcode, &initial.from, NULL, NULL)) {
        lx_errno = LX_EINVAL;
        return (void *)-1;
    }
    initial.input_be = initial.from.be;
    cd = lx_malloc(sizeof *cd);
    if (!cd)
        return (void *)-1;
    *cd = initial;
    return cd;
}

static size_t iconv_(iconv_descriptor *cd, char **inbuf, size_t *inbytesleft,
                     char **outbuf, size_t *outbytesleft)
{
    size_t irreversible = 0;
    int ignored_error = 0;
    if (!cd || cd == (void *)-1)
        return iconv_error(LX_EBADF);
    if (!inbuf || !*inbuf) {
        /* None of the supported encodings has a trailing shift sequence.
         * Reset re-arms BOM handling, but glibc retains its byte-swap flag. */
        cd->input_started = cd->output_started = 0;
        return 0;
    }
    while (*inbytesleft) {
        const unsigned char *in = (const unsigned char *)*inbuf;
        unsigned char encoded[128];
        uint32_t cp;
        size_t bad = 1;
        int consumed, produced;
        if (!cd->input_started && cd->from.bom) {
            unsigned width = cd->from.kind == ENC_UTF16 ? 2 : 4;
            uint32_t mark;
            if (*inbytesleft < width)
                return iconv_error(LX_EINVAL);
            mark = width == 2 ? get16(in, 0) : get32(in, 0);
            cd->input_started = 1;
            if (mark == 0xfeff || mark == (width == 2 ? 0xfffeU : 0xfffe0000U)) {
                if (mark != 0xfeff)
                    cd->input_be = 1;
                *inbuf += width;
                *inbytesleft -= width;
                continue;
            }
        }
        consumed = decode(cd->from, cd->input_be, in, *inbytesleft, &cp, &bad);
        if (consumed == -2)
            return iconv_error(LX_EINVAL);
        if (consumed < 0) {
            if (!cd->ignore)
                return iconv_error(LX_EILSEQ);
            *inbuf += bad;
            *inbytesleft -= bad;
            ignored_error = 1;
            continue;
        }
        if (!cd->output_started && cd->to.bom) {
            unsigned width = cd->to.kind == ENC_UTF16 ? 2 : 4;
            if (*outbytesleft < width)
                return iconv_error(LX_E2BIG);
            if (width == 2)
                put16((unsigned char *)*outbuf, 0xfeff, 0);
            else
                put32((unsigned char *)*outbuf, 0xfeff, 0);
            *outbuf += width;
            *outbytesleft -= width;
            cd->output_started = 1;
        }
        /* Even an unrepresentable character needs the destination's minimum
         * width before the target converter can report EILSEQ. */
        {
            unsigned minimum = cd->to.kind == ENC_UTF16 || cd->to.kind == ENC_UCS2 ? 2 :
                cd->to.kind == ENC_UTF32 || cd->to.kind == ENC_UCS4 || cd->to.kind == ENC_WCHAR ? 4 : 1;
            if (*outbytesleft < minimum)
                return iconv_error(LX_E2BIG);
        }
        produced = encode(cd->to, cp, encoded);
        /* glibc drops Unicode language tags when a narrow repertoire cannot
         * represent them; unlike //IGNORE this is not an error or a loss count. */
        if (produced < 0 && cp >= 0xe0000 && cp <= 0xe007f &&
            (cd->to.kind == ENC_ASCII || cd->to.kind == ENC_LATIN1 ||
             cd->to.kind == ENC_UCS2 || cd->to.kind >= ENC_CP949))
            produced = 0;
        if (produced < 0) {
            /* A malformed UCS scalar is not a transliteration opportunity. */
            int scalar_error = (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x7fffffff;
            if (cd->translit && !scalar_error) {
                produced = transliterate(cd->to, cp, encoded);
                ++irreversible;
            }
            if (produced < 0) {
                if (!cd->ignore)
                    return iconv_error(LX_EILSEQ);
                *inbuf += consumed;
                *inbytesleft -= (size_t)consumed;
                ignored_error = 1;
                continue;
            }
        }
        if ((size_t)produced > *outbytesleft)
            return iconv_error(LX_E2BIG);
        memcpy(*outbuf, encoded, (size_t)produced);
        *outbuf += produced;
        *outbytesleft -= (size_t)produced;
        *inbuf += consumed;
        *inbytesleft -= (size_t)consumed;
        cd->input_started = 1;
    }
    /* glibc //IGNORE consumes bad input but still reports the conversion error. */
    return ignored_error ? iconv_error(LX_EILSEQ) : irreversible;
}

static int iconv_close_(iconv_descriptor *cd)
{
    if (!cd || cd == (void *)-1)
        return lx_set_errno(LX_EBADF);
    lx_free(cd);
    return 0;
}

LX_SYMTAB(iconv,
    LX_SYM("iconv_open", iconv_open_),
    LX_SYM("iconv", iconv_),
    LX_SYM("iconv_close", iconv_close_),
);
