#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* M1 acceptance: -fstack-protector-all forces %gs:0x14 canary code in every function. */
static int fill(char *dst, const char *src)
{
    char local[32];
    strncpy(local, src, sizeof local - 1);
    local[sizeof local - 1] = 0;
    strcpy(dst, local);
    return (int)strlen(dst);
}

int main(int argc, char **argv)
{
    char buf[64];
    int n = fill(buf, "hello from relinked i386 ELF");
    puts(buf);
    if (argc > 1)
        return atoi(argv[1]);
    return n;
}
