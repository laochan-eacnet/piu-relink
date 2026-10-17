#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void at_exit(void)
{
    fputs("atexit-out|", stdout);
    fputs("atexit-err|", stderr);
}
int main(void)
{
    /* User buffers must outlive exit's final flush. */
    static char buf[128];
    setvbuf(stdout, buf, _IOFBF, sizeof buf);
    fputs("out1|", stdout);
    fputs("err1|", stderr);
    fflush(NULL);
    fputs("out2|", stdout);
    fputs("err2|", stderr);
    fflush(stdout);
    /* setvbuf is called before any I/O on each stream, as required by C. */
    FILE *line = fdopen(dup(STDOUT_FILENO), "w");
    setvbuf(line, NULL, _IOLBF, 0);
    fputs("line-buffered\n", line); fclose(line);
    fputs("err3|", stderr);
    FILE *none = fdopen(dup(STDOUT_FILENO), "w");
    setbuf(none, NULL);
    fputs("unbuffered|", none); fclose(none);
    fputs("err4|", stderr);
    atexit(at_exit);
    fputs("exit-tail|", stdout);
    return 0;
}
