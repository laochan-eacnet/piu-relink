#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wchar.h>
#include <locale.h>

/* Bypass glibc's header-only FILE-layout optimization: lxrt FILE is opaque. */
extern int test_getc(FILE *) __asm__("getc_unlocked");
extern int test_putc(int, FILE *) __asm__("putc_unlocked");
extern int test_feof(FILE *) __asm__("feof_unlocked");
extern int test_ferror(FILE *) __asm__("ferror_unlocked");
extern void test_clearerr(FILE *) __asm__("clearerr_unlocked");

static void dump(const char *name, const void *p, size_t n)
{
    const unsigned char *b = p;
    printf("%s:", name);
    for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
    putchar('\n');
}
int main(void)
{
    _Static_assert(sizeof(fpos_t) == 12, "i386 fpos_t");
    _Static_assert(sizeof(fpos64_t) == 16, "i386 fpos64_t");
    FILE *f = tmpfile();
    if (!f) { perror("tmpfile"); return 1; }
    char buf[19];
    printf("setvbuf=%d\n", setvbuf(f, buf, _IOFBF, sizeof buf));
    printf("write=%zu\n", fwrite("alpha\nbeta\nlast", 1, 15, f));
    printf("tell=%ld flush=%d\n", ftell(f), fflush(f));
    rewind(f);
    char line[32];
    char *p = fgets(line, sizeof line, f);
    printf("line=%s", p ? p : "NULL\n");
    printf("tell-line=%ld\n", ftell(f));
    fpos_t pos;
    printf("getpos=%d\n", fgetpos(f, &pos));
    for (int i = 0; i < 8; ++i) printf("push%d=%d\n", i, ungetc('0' + i, f));
    char pushes[9] = {0};
    fread(pushes, 1, 8, f); printf("pushes=%s tell=%ld\n", pushes, ftell(f));
    fsetpos(f, &pos);
    char *dynamic = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&dynamic, &cap, f)) >= 0) { printf("getline=%zd:", n); dump("data", dynamic, n); }
    printf("eof=%d error=%d\n", !!feof(f), !!ferror(f));
    ungetc('Q', f); printf("un-eof=%d char=%c\n", !!feof(f), fgetc(f));
    clearerr(f); printf("clear=%d/%d\n", !!feof(f), !!ferror(f));
    free(dynamic);
    fseeko64(f, 2, SEEK_SET); printf("seek64=%lld\n", (long long)ftello64(f));
    fpos64_t p64; fgetpos64(f, &p64); fseek(f, 0, SEEK_END); fsetpos64(f, &p64);
    printf("position64=%lld char=%c\n", (long long)ftello64(f), fgetc(f));
    fseek(f, 0, SEEK_SET);
    printf("lock=%d\n", ftrylockfile(f)); flockfile(f); funlockfile(f); funlockfile(f);
    printf("unlocked=%c", test_getc(f)); test_clearerr(f); printf("/%d/%d\n", !!test_feof(f), !!test_ferror(f));
    fseek(f, 0, SEEK_END); test_putc('!', f); fflush_unlocked(f);
    fclose(f);

    const char *path = "/tmp/lxrt-stdio-roundtrip.tmp";
    f = fopen(path, "w+be");
    if (!f) { perror("fopen"); return 2; }
    setlinebuf(f); fputs("one\ntwo\n", f); fclose(f);
    f = fopen64(path, "a+b");
    printf("append-initial=%ld\n", ftell(f));
    fseek(f, 0, SEEK_SET); fwrite("three\n", 1, 6, f); fflush(f);
    printf("append-final=%ld\n", ftell(f)); rewind(f);
    char data[128] = {0}; n = fread(data, 1, sizeof data, f); dump("append", data, n); fclose(f);
    int fd = open(path, O_RDWR);
    f = fdopen(fd, "r+");
    printf("fdopen=%d\n", f != NULL && fileno(f) == fd);
    setbuffer(f, buf, sizeof buf); fseek(f, 0, SEEK_END); fputs("four\n", f);
    f = freopen(path, "r", f);
    printf("freopen=%d\n", f != NULL);
    n = fread(data, 1, sizeof data, f); dump("reopen", data, n);
    errno = 0; size_t bad = fwrite("x", 1, 1, f); printf("readonly=%zu/%d/%d\n", bad, !!ferror(f), errno); fclose(f);
    f = fopen(path, "wx"); printf("exclusive=%d/%d\n", f == NULL, errno); if (f) fclose(f);
    unlink(path);

    char memory[24]; memset(memory, 0x7e, sizeof memory);
    f = fmemopen(memory, sizeof memory, "w+");
    fputs("abc\ndef", f); fflush(f); dump("fmem", memory, 12);
    rewind(f); n = fread(data, 1, sizeof data, f); dump("fmemread", data, n);
    fclose(f);
    f = fmemopen(memory, sizeof memory, "a+"); fseek(f, 0, SEEK_SET); fputs("G", f); fflush(f); dump("fmemappend", memory, 12); fclose(f);
    char *mem = NULL; size_t len = 0;
    f = open_memstream(&mem, &len); fputs("stream", f); fflush(f); dump("memstream", mem, len);
    fseek(f, 10, SEEK_SET); fputc('X', f); fflush(f); dump("memhole", mem, len);
    fclose(f); free(mem);
    f = open_memstream(&mem, &len); fputs("abcdef", f); fflush(f);
    fseek(f, 2, SEEK_SET); fflush(f); dump("memback", mem, len);
    fseek(f, 5, SEEK_SET); fflush(f); dump("memforward", mem, len);
    fseek(f, 0, SEEK_END); fputc('Z', f); fclose(f); dump("memend", mem, len); free(mem);
    for (size_t limit = 0; limit <= 8; ++limit) {
        memset(memory, '~', sizeof memory);
        f = fmemopen(memory, limit, "w+");
        errno = 0; size_t written = fwrite("0123456789", 1, 10, f);
        int flushed = fflush(f), saved = errno;
        printf("memlimit%zu=%zu/%d/%d/%d\n", limit, written, flushed, !!ferror(f), saved);
        dump("memlimit", memory, limit + 1); fclose(f);
    }
    f = fmemopen("one|two||end", 12, "r"); dynamic = NULL; cap = 0;
    while ((n = getdelim(&dynamic, &cap, '|', f)) >= 0) dump("delim", dynamic, n);
    free(dynamic); fclose(f);

    setlocale(LC_ALL, "C.UTF-8");
    f = tmpfile(); printf("wide0=%d\n", fwide(f, 0)); printf("wide1=%d\n", fwide(f, 1) > 0);
    fputwc(L'A', f); putwc(0x20ac, f); fputwc(0x1f642, f); rewind(f);
    wint_t w1 = fgetwc(f), w2 = getwc(f), w3 = fgetwc(f);
    printf("wide=%x/%x/%x\n", (unsigned)w1, (unsigned)w2, (unsigned)w3);
    ungetwc(0x03a9, f); printf("widepush=%x\n", (unsigned)fgetwc(f)); fclose(f);
    FILE *original = stdout;
    f = tmpfile(); stdout = f;
    printf("copy-%d", 17); puts("-puts"); putchar('!');
    fflush(stdout); stdout = original; rewind(f);
    n = fread(data, 1, sizeof data, f); dump("copied-stdout", data, n); fclose(f);
    original = stdin; f = fmemopen("Z42", 3, "r"); stdin = f;
    int input = getchar(), number = 0; int assigned = scanf("%d", &number);
    stdin = original; fclose(f); printf("copied-stdin=%c/%d/%d\n", input, assigned, number);
    original = stderr; f = tmpfile(); stderr = f; errno = ENOENT; perror("copy");
    fflush(stderr); stderr = original; rewind(f);
    n = fread(data, 1, sizeof data, f); dump("copied-stderr", data, n); fclose(f);
    puts("file-done");
    return 0;
}
