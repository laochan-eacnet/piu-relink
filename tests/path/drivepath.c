/* Regression: bare drive paths ("D:") and progressive mkdir of a drive path (phoenixtools'
 * create_directory_tree) must not overrun lxrt's path normalizer. argv[0] is a drive path. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    char drive[4] = { argv[0][0], ':', 0, 0 };
    if (argv[0][1] != ':') {
        puts("argv[0] is not a drive path");
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < 2000; i++) {   /* repeat: the old bug read past the string end */
        struct stat st;
        if (stat(drive, &st) != 0 || !S_ISDIR(st.st_mode))
            bad++;
        if (mkdir(drive, 0755) == 0 || errno != EEXIST)
            bad++;
    }
    char dir[512];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    *strrchr(dir, '/') = 0;
    /* walk every prefix like create_directory_tree */
    for (char *p = dir + 2; (p = strchr(p, '/')); p++) {
        *p = 0;
        if (mkdir(dir[2] ? dir : drive, 0755) != 0 && errno != EEXIST)
            bad++;
        *p = '/';
    }
    printf("drive paths: %s\n", bad ? "BAD" : "ok");
    return bad != 0;
}
