/* Deterministic glibc/i386 iconv comparison. Build as a guest and run unchanged
 * under native Linux/qemu and lxrt; all vectors are bytes, never host wchar_t.
 * Success prints one stable line; failures report the exact assertion. */
#include <errno.h>
#include <iconv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "iconv:%d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

static const unsigned char korean_utf8[] = { 'A', 0xed, 0x95, 0x9c, 0xea, 0xb8, 0x80, 0, 'Z' };
static const unsigned char korean_utf16le[] = { 'A', 0, 0x5c, 0xd5, 0, 0xae, 0, 0, 'Z', 0 };
static const unsigned char korean_cp949[] = { 'A', 0xc7, 0xd1, 0xb1, 0xdb, 0, 'Z' };

static void exact(const char *to, const char *from, const void *input, size_t input_size,
                  const void *expected, size_t expected_size)
{
    unsigned char output[256];
    char *in = (char *)input, *out = (char *)output;
    size_t left = input_size, room = sizeof output;
    iconv_t cd = iconv_open(to, from);
    CHECK(cd != (iconv_t)-1);
    memset(output, 0xcc, sizeof output);
    CHECK(iconv(cd, &in, &left, &out, &room) == 0);
    CHECK(left == 0 && in == (char *)input + input_size);
    CHECK(out == (char *)output + expected_size && room == sizeof output - expected_size);
    CHECK(memcmp(output, expected, expected_size) == 0);
    CHECK(output[expected_size] == 0xcc);
    CHECK(iconv(cd, NULL, NULL, &out, &room) == 0);
    CHECK(out == (char *)output + expected_size);
    CHECK(iconv_close(cd) == 0);
}

#define EXACT(to, from, in, expected) exact(to, from, in, sizeof(in), expected, sizeof(expected))

static void korean_conversions(void)
{
    static const unsigned char uhc_utf8[] = { 0xea, 0xb0, 0x82 };
    static const unsigned char uhc_bytes[] = { 0x81, 0x41 };
    EXACT("UTF-16LE", "UTF-8", korean_utf8, korean_utf16le);
    EXACT("UTF-8", "UTF-16LE", korean_utf16le, korean_utf8);
    EXACT("CP949", "UTF-16LE", korean_utf16le, korean_cp949);
    EXACT("UTF-16LE", "CP949", korean_cp949, korean_utf16le);
    EXACT("CP949", "UTF-8", korean_utf8, korean_cp949);
    EXACT("UTF-8", "CP949", korean_cp949, korean_utf8);
    EXACT("EUC-KR", "UTF-8", korean_utf8, korean_cp949);
    EXACT("UTF-8", "EUC-KR", korean_cp949, korean_utf8);
    EXACT("UHC", "UTF8", uhc_utf8, uhc_bytes);
    EXACT("UTF8", "UHC", uhc_bytes, uhc_utf8);
}

static void encoding_vectors(void)
{
    static const unsigned char unicode[] = { 'A', 0xc3, 0xa9 };
    static const unsigned char latin[] = { 'A', 0xe9 };
    static const unsigned char le16[] = { 'A', 0, 0xe9, 0 };
    static const unsigned char be16[] = { 0, 'A', 0, 0xe9 };
    static const unsigned char le32[] = { 'A', 0, 0, 0, 0xe9, 0, 0, 0 };
    static const unsigned char be32[] = { 0, 0, 0, 'A', 0, 0, 0, 0xe9 };
    static const unsigned char bom16[] = { 0xff, 0xfe, 'A', 0, 0xe9, 0 };
    static const unsigned char bom32[] = { 0xff, 0xfe, 0, 0, 'A', 0, 0, 0, 0xe9, 0, 0, 0 };
    static const unsigned char big_bom16[] = { 0xfe, 0xff, 0, 'A', 0, 0xe9 };
    static const unsigned char big_bom32[] = { 0, 0, 0xfe, 0xff, 0, 0, 0, 'A', 0, 0, 0, 0xe9 };
    static const unsigned char emoji[] = { 0xf0, 0x9f, 0x98, 0x80 };
    static const unsigned char emoji16[] = { 0x3d, 0xd8, 0, 0xde };
    static const unsigned char historical_utf8[] = { 0xf8, 0x88, 0x80, 0x80, 0x80 };
    static const unsigned char historical_ucs4[] = { 0, 0x20, 0, 0 };
    static const unsigned char euro_quotes[] = { 0xe2, 0x82, 0xac, 0xe2, 0x80, 0x9c, 'A', 0xe2, 0x80, 0x9d };
    static const unsigned char cp1252[] = { 0x80, 0x93, 'A', 0x94 };
    static const unsigned char japanese[] = { 0xe3, 0x81, 0x82, 0xc2, 0xa5, 0xe2, 0x80, 0xbe };
    static const unsigned char sjis[] = { 0x82, 0xa0, 0x5c, 0x7e };
    static const unsigned char chinese[] = { 0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87 };
    static const unsigned char gbk[] = { 0xd6, 0xd0, 0xce, 0xc4 };
    static const unsigned char ascii[] = { 'A', 0, 'Z' };
    static const unsigned char jis_cells[] = { 0x81, 0x60, 0x81, 0x61, 0x81, 0x7c };
    static const unsigned char jis_unicode[] = { 0xe3, 0x80, 0x9c, 0xe2, 0x80, 0x96, 0xe2, 0x88, 0x92 };
    static const unsigned char c1[] = { 0x80, 0x81, 0x9f };
    static const unsigned char c1_utf8[] = { 0xc2, 0x80, 0xc2, 0x81, 0xc2, 0x9f };
    static const unsigned char won_utf8[] = { 0xe2, 0x82, 0xa9 };
    static const unsigned char fullwidth_won[] = { 0xa3, 0xdc };
    EXACT("LATIN1", "UTF8", unicode, latin);
    EXACT("UTF8", "ISO-8859-1", latin, unicode);
    EXACT("UCS-2", "UTF8", unicode, le16);
    EXACT("UCS-2LE", "UTF8", unicode, le16);
    EXACT("UCS-2BE", "UTF8", unicode, be16);
    EXACT("UTF8", "UCS-2", le16, unicode);
    EXACT("UTF8", "UCS-2BE", be16, unicode);
    EXACT("UTF-16BE", "UTF8", unicode, be16);
    EXACT("UTF8", "UTF-16BE", be16, unicode);
    EXACT("UTF-16", "UTF8", unicode, bom16);
    EXACT("UTF8", "UTF-16", bom16, unicode);
    EXACT("UTF8", "UTF-16", big_bom16, unicode);
    EXACT("UCS-4", "UTF8", unicode, be32);
    EXACT("UCS-4BE", "UTF8", unicode, be32);
    EXACT("UCS-4LE", "UTF8", unicode, le32);
    EXACT("WCHAR_T", "UTF8", unicode, le32);
    EXACT("UTF8", "UCS-4", be32, unicode);
    EXACT("UTF8", "UCS-4LE", le32, unicode);
    EXACT("UTF8", "WCHAR_T", le32, unicode);
    EXACT("UTF-32LE", "UTF8", unicode, le32);
    EXACT("UTF-32BE", "UTF8", unicode, be32);
    EXACT("UTF-32", "UTF8", unicode, bom32);
    EXACT("UTF8", "UTF-32", bom32, unicode);
    EXACT("UTF8", "UTF-32", big_bom32, unicode);
    EXACT("UTF8", "UTF-32LE", le32, unicode);
    EXACT("UTF8", "UTF-32BE", be32, unicode);
    EXACT("UTF-16LE", "UTF8", emoji, emoji16);
    EXACT("UTF8", "UTF-16LE", emoji16, emoji);
    EXACT("UCS-4", "UTF8", historical_utf8, historical_ucs4);
    EXACT("UTF8", "UCS-4", historical_ucs4, historical_utf8);
    EXACT("CP1252", "UTF8", euro_quotes, cp1252);
    EXACT("UTF8", "WINDOWS-1252", cp1252, euro_quotes);
    EXACT("SHIFT_JIS", "UTF8", japanese, sjis);
    EXACT("UTF8", "SHIFT_JIS", sjis, japanese);
    EXACT("UTF8", "SHIFT_JIS", jis_cells, jis_unicode);
    EXACT("SHIFT_JIS", "UTF8", jis_unicode, jis_cells);
    EXACT("GBK", "UTF8", chinese, gbk);
    EXACT("UTF8", "GBK", gbk, chinese);
    EXACT("US-ASCII", "UTF8", ascii, ascii);
    EXACT("UTF8", "ASCII", ascii, ascii);
    EXACT("UTF8", "EUC-KR", c1, c1_utf8);
    EXACT("EUC-KR", "UTF8", c1_utf8, c1);
    EXACT("EUC-KR", "UTF8", won_utf8, fullwidth_won);
    /* Missing BOM means native little endian, not an implicit big-endian stream. */
    EXACT("UTF8", "UTF-16", le16, unicode);
    EXACT("UTF8", "UTF-32", le32, unicode);
}

static void error_case(const char *to, const char *from, const void *input, size_t size,
                       size_t capacity, int error, size_t consumed, const void *expected, size_t written)
{
    unsigned char output[128];
    char *in = (char *)input, *out = (char *)output;
    size_t left = size, room = capacity;
    iconv_t cd = iconv_open(to, from);
    CHECK(cd != (iconv_t)-1);
    memset(output, 0xcc, sizeof output);
    errno = 0;
    CHECK(iconv(cd, &in, &left, &out, &room) == (size_t)-1);
    CHECK(errno == error);
    CHECK(in == (char *)input + consumed && left == size - consumed);
    CHECK(out == (char *)output + written && room == capacity - written);
    CHECK(!written || memcmp(output, expected, written) == 0);
    CHECK(output[written] == 0xcc);
    CHECK(iconv_close(cd) == 0);
}

static void error_vectors(void)
{
    static const unsigned char utf8_prefix[] = { 'A', 0xed, 0x95, 0x9c };
    static const unsigned char prefix[] = { 'A', 0 };
    static const unsigned char partial_utf8[] = { 0xed, 0x95 };
    static const unsigned char invalid_utf8[] = { 'A', 0xc0, 0x80 };
    static const unsigned char invalid_continuation[] = { 0xed, 'X' };
    static const unsigned char partial_cp949[] = { 0xc7 };
    static const unsigned char invalid_cp949[] = { 0xc7, '!' };
    static const unsigned char partial_sjis[] = { 0x82 };
    static const unsigned char partial_gbk[] = { 0xd6 };
    static const unsigned char invalid_euckr[] = { 0xa0, 0x41 };
    static const unsigned char partial_utf16[] = { 0x3d, 0xd8, 0 };
    static const unsigned char invalid_utf16[] = { 0x3d, 0xd8, 'A', 0 };
    static const unsigned char low_surrogate[] = { 0, 0xdc };
    static const unsigned char odd_utf16[] = { 'A' };
    static const unsigned char invalid_utf32[] = { 0, 0, 0x11, 0 };
    static const unsigned char partial_utf32[] = { 'A', 0, 0 };
    static const unsigned char invalid_cp1252[] = { 0x81 };
    static const unsigned char invalid_ascii[] = { 0x80 };
    static const unsigned char korean[] = { 0xed, 0x95, 0x9c };
    static const unsigned char cp949[] = { 0xc7, 0xd1 };
    static const unsigned char emoji[] = { 0xf0, 0x9f, 0x98, 0x80 };
    static const unsigned char euro[] = { 0xe2, 0x82, 0xac };
    error_case("UTF-16LE", "UTF8", utf8_prefix, sizeof utf8_prefix, 3, E2BIG, 1, prefix, 2);
    error_case("CP949", "UTF8", korean, sizeof korean, 1, E2BIG, 0, NULL, 0);
    error_case("UTF8", "CP949", cp949, sizeof cp949, 2, E2BIG, 0, NULL, 0);
    error_case("UTF8", "UTF8", partial_utf8, sizeof partial_utf8, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "UTF8", invalid_utf8, sizeof invalid_utf8, 32, EILSEQ, 1, "A", 1);
    error_case("UTF8", "UTF8", invalid_continuation, sizeof invalid_continuation, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "CP949", partial_cp949, sizeof partial_cp949, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "CP949", invalid_cp949, sizeof invalid_cp949, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "SHIFT_JIS", partial_sjis, sizeof partial_sjis, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "GBK", partial_gbk, sizeof partial_gbk, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "EUC-KR", invalid_euckr, sizeof invalid_euckr, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "UTF-16LE", partial_utf16, sizeof partial_utf16, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "UTF-16LE", invalid_utf16, sizeof invalid_utf16, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "UTF-16LE", low_surrogate, sizeof low_surrogate, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "UTF-16LE", odd_utf16, sizeof odd_utf16, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "UTF-32LE", invalid_utf32, sizeof invalid_utf32, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "UTF-32LE", partial_utf32, sizeof partial_utf32, 32, EINVAL, 0, NULL, 0);
    error_case("UTF8", "CP1252", invalid_cp1252, sizeof invalid_cp1252, 32, EILSEQ, 0, NULL, 0);
    error_case("UTF8", "ASCII", invalid_ascii, sizeof invalid_ascii, 32, EILSEQ, 0, NULL, 0);
    error_case("ASCII", "UTF8", korean, sizeof korean, 32, EILSEQ, 0, NULL, 0);
    error_case("ISO8859-1", "UTF8", euro, sizeof euro, 32, EILSEQ, 0, NULL, 0);
    error_case("CP1252", "UTF8", korean, sizeof korean, 32, EILSEQ, 0, NULL, 0);
    error_case("CP949", "UTF8", emoji, sizeof emoji, 32, EILSEQ, 0, NULL, 0);
    error_case("UCS-2", "UTF8", emoji, sizeof emoji, 32, EILSEQ, 0, NULL, 0);
}

static void retry_and_bom_state(void)
{
    unsigned char output[32];
    static const unsigned char input[] = { 'A', 0xed, 0x95, 0x9c };
    static const unsigned char expected[] = { 'A', 0, 0x5c, 0xd5 };
    static const unsigned char big16[] = { 0xfe, 0xff, 0, 'A', 0, 'B' };
    static const unsigned char mark16[] = { 0xff, 0xfe };
    static const unsigned char mark_utf8[] = { 0xef, 0xbb, 0xbf };
    char *in = (char *)input, *out = (char *)output;
    size_t left = sizeof input, room = 3;
    iconv_t cd = iconv_open("UTF-16LE", "UTF8");
    CHECK(cd != (iconv_t)-1);
    CHECK(iconv(cd, &in, &left, &out, &room) == (size_t)-1 && errno == E2BIG);
    CHECK(in == (char *)input + 1 && left == 3 && out == (char *)output + 2 && room == 1);
    room = 2;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0);
    CHECK(left == 0 && room == 0 && !memcmp(output, expected, sizeof expected));
    CHECK(iconv_close(cd) == 0);

    cd = iconv_open("UTF-16", "UTF8");
    CHECK(cd != (iconv_t)-1);
    in = "A"; left = 1; out = (char *)output; room = 1;
    CHECK(iconv(cd, &in, &left, &out, &room) == (size_t)-1 && errno == E2BIG);
    CHECK(left == 1 && out == (char *)output && room == 1);
    room = 2;
    CHECK(iconv(cd, &in, &left, &out, &room) == (size_t)-1 && errno == E2BIG);
    CHECK(left == 1 && out == (char *)output + 2 && room == 0);
    CHECK(output[0] == 0xff && output[1] == 0xfe);
    room = 2;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0);
    CHECK(left == 0 && out == (char *)output + 4 && output[2] == 'A' && output[3] == 0);
    room = 8;
    CHECK(iconv(cd, NULL, NULL, &out, &room) == 0 && room == 8);
    in = "B"; left = 1;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && room == 4);
    CHECK(output[4] == 0xff && output[5] == 0xfe && output[6] == 'B' && output[7] == 0);
    in = NULL;
    CHECK(iconv(cd, &in, NULL, NULL, NULL) == 0);
    in = "C"; left = 1; room = 4;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && room == 0);
    CHECK(output[8] == 0xff && output[9] == 0xfe && output[10] == 'C' && output[11] == 0);
    CHECK(iconv_close(cd) == 0);

    cd = iconv_open("UTF8", "UTF-16");
    CHECK(cd != (iconv_t)-1);
    in = (char *)big16; left = 4; out = (char *)output; room = sizeof output;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && left == 0);
    CHECK(out == (char *)output + 1 && output[0] == 'A');
    left = 2;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && left == 0);
    CHECK(out == (char *)output + 2 && output[1] == 'B');
    CHECK(iconv(cd, NULL, NULL, NULL, NULL) == 0);
    in = (char *)big16; left = sizeof big16; out = (char *)output; room = sizeof output;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && left == 0);
    CHECK(out == (char *)output + 2 && !memcmp(output, "AB", 2));
    CHECK(iconv_close(cd) == 0);
    /* Explicit endianness preserves U+FEFF rather than treating it as metadata. */
    EXACT("UTF8", "UTF-16LE", mark16, mark_utf8);
    cd = iconv_open("UTF8", "UTF-16");
    CHECK(cd != (iconv_t)-1);
    in = (char *)mark16; left = sizeof mark16; out = (char *)output; room = 0;
    CHECK(iconv(cd, &in, &left, &out, &room) == 0 && left == 0 && out == (char *)output);
    CHECK(iconv_close(cd) == 0);
}

static void suffixes_and_open_errors(void)
{
    static const unsigned char nonrepresentable[] = { 'A', 0xc3, 0xa9, 'B' };
    static const unsigned char malformed[] = { 'A', 0xff, 'B' };
    static const unsigned char partial[] = { 'A', 0xed, 0x95 };
    static const unsigned char translit_input[] = { 0xc3, 0xa9, 0xed, 0x95, 0x9c };
    unsigned char output[16];
    char *in = (char *)translit_input, *out = (char *)output;
    size_t left = sizeof translit_input, room = sizeof output;
    iconv_t cd;
    error_case("ASCII//IGNORE", "UTF8", nonrepresentable, sizeof nonrepresentable, 16,
               EILSEQ, sizeof nonrepresentable, "AB", 2);
    error_case("ASCII//IGNORE", "UTF8", malformed, sizeof malformed, 16,
               EILSEQ, sizeof malformed, "AB", 2);
    error_case("ASCII//IGNORE", "UTF8", partial, sizeof partial, 16, EINVAL, 1, "A", 1);
    /* A source suffix does not turn on the destination's ignore policy. */
    error_case("ASCII", "UTF8//IGNORE", nonrepresentable, sizeof nonrepresentable, 16, EILSEQ, 1, "A", 1);
    CHECK(setlocale(LC_CTYPE, "C") != NULL);
    cd = iconv_open("ASCII//TRANSLIT", "UTF8");
    CHECK(cd != (iconv_t)-1);
    CHECK(iconv(cd, &in, &left, &out, &room) == 2);
    CHECK(left == 0 && out == (char *)output + 2 && !memcmp(output, "??", 2));
    CHECK(iconv_close(cd) == 0);
    cd = iconv_open("ASCII//TRANSLIT,IGNORE", "UTF8");
    CHECK(cd != (iconv_t)-1);
    in = (char *)translit_input; left = sizeof translit_input; out = (char *)output; room = sizeof output;
    CHECK(iconv(cd, &in, &left, &out, &room) == 2 && left == 0);
    CHECK(iconv_close(cd) == 0);
    errno = 0;
    CHECK(iconv_open("NO-SUCH-ENCODING", "UTF8") == (iconv_t)-1 && errno == EINVAL);
    errno = 0;
    CHECK(iconv_open("UTF8", "NO-SUCH-ENCODING") == (iconv_t)-1 && errno == EINVAL);
    errno = 0;
    CHECK(iconv((iconv_t)-1, NULL, NULL, NULL, NULL) == (size_t)-1 && errno == EBADF);
    errno = 0;
    CHECK(iconv_close((iconv_t)-1) == -1 && errno == EBADF);
}

int main(void)
{
    CHECK(setlocale(LC_ALL, "C") != NULL);
    korean_conversions();
    encoding_vectors();
    error_vectors();
    retry_and_bom_state();
    suffixes_and_open_errors();
    puts("iconv: ok");
    return 0;
}
