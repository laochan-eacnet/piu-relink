#pragma once
#include "lxrt.h"

/* glibc 2.31 i386 locale_t and mbstate_t; never use the UCRT wide ABI. */
typedef struct lx_locale {
    void *categories[13];
    const uint16_t *ctype_b;
    const int32_t *ctype_tolower;
    const int32_t *ctype_toupper;
    const char *names[13];
} lx_locale;
_Static_assert(sizeof(lx_locale) == 116, "glibc locale_t size");
_Static_assert(offsetof(lx_locale, ctype_b) == 52, "glibc locale_t ctype");
_Static_assert(offsetof(lx_locale, ctype_tolower) == 56, "glibc locale_t tolower");
_Static_assert(offsetof(lx_locale, ctype_toupper) == 60, "glibc locale_t toupper");
_Static_assert(offsetof(lx_locale, names) == 64, "glibc locale_t names");
typedef struct lx_mbstate { uint32_t count, value; } lx_mbstate;
_Static_assert(sizeof(lx_mbstate) == 8, "glibc mbstate_t size");
_Static_assert(offsetof(lx_mbstate, value) == 4, "glibc mbstate_t value");

void lx_ctype_tables(const uint16_t **b, const int32_t **lo, const int32_t **up);
lx_locale *lx_locale_current(void);
int lx_locale_utf8(const lx_locale *locale);
size_t lx_mbrtowc(uint32_t *wc, const char *s, size_t n, lx_mbstate *state);
size_t lx_wcrtomb(char *s, uint32_t wc, lx_mbstate *state);
size_t lx_wcslen(const uint32_t *s);
int lx_wcscmp(const uint32_t *a, const uint32_t *b);
size_t lx_wcsxfrm(uint32_t *dst, const uint32_t *src, size_t n);
/* Stateless UTF-8 codec: -1 malformed, -2 incomplete; otherwise consumed bytes. */
int lx_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp);
int lx_utf8_encode(uint32_t cp, char *s);
