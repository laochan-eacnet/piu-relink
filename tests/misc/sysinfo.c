/* System information files a game status screen reads: getifaddrs eth0 matches /proc/net/route
 * and /sys/class/net/eth0, CPU/mainboard strings are printable. Self-checking; host-specific
 * values are printed, not compared. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return ok;
}

static int printable(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f)
            return 0;
    return 1;
}

int main(void)
{
    struct ifaddrs *list;
    char ip[32] = "", line[512], state[32];
    int v4_done = 0, order_ok = 1;
    CHECK(getifaddrs(&list) == 0);
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr)
            continue;
        if (p->ifa_addr->sa_family == AF_INET6)
            v4_done = 1;
        else if (p->ifa_addr->sa_family == AF_INET && v4_done)
            order_ok = 0;
        if (p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, "eth0") && !ip[0])
            strcpy(ip, inet_ntoa(((struct sockaddr_in *)p->ifa_addr)->sin_addr));
    }
    freeifaddrs(list);
    CHECK(order_ok); /* glibc: all IPv4 entries before IPv6 */
    printf("eth0 ip=%s\n", ip[0] ? ip : "(none)");

    FILE *f = fopen("/proc/net/route", "r");
    CHECK(f != NULL);
    int header = 0, default_eth0 = 0;
    while (f && fgets(line, sizeof line, f)) {
        char ifn[32];
        unsigned long dest, gw;
        if (!header++) {
            CHECK(!strncmp(line, "Iface\tDestination\tGateway", 25));
            continue;
        }
        CHECK(sscanf(line, "%31s %lx %lx", ifn, &dest, &gw) == 3);
        if (!strcmp(ifn, "eth0") && dest == 0 && gw) {
            struct in_addr a = { (in_addr_t)gw };
            printf("eth0 gateway=%s\n", inet_ntoa(a));
            default_eth0 = 1;
        }
    }
    if (f)
        fclose(f);
    if (ip[0])
        CHECK(default_eth0); /* eth0 is the adapter with the default route */

    CHECK(read_line("/sys/class/net/eth0/operstate", state, sizeof state));
    CHECK(!strcmp(state, "up") || !strcmp(state, "down") || !strcmp(state, "unknown"));
    CHECK(read_line("/sys/class/net/eth0/carrier", line, sizeof line) && (!strcmp(line, "1") || !strcmp(line, "0")));
    CHECK(read_line("/sys/class/net/eth0/address", line, sizeof line) && strlen(line) == 17 && line[2] == ':');
    printf("eth0 operstate=%s mac=%s\n", state, line);
    CHECK(fopen("/sys/class/net/eth0/nonexistent", "r") == NULL);
    CHECK(fopen("/sys/class/net/nosuchif/operstate", "r") == NULL);

    int model = 0;
    f = fopen("/proc/cpuinfo", "r");
    CHECK(f != NULL);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "model name", 10) && !model++) {
            line[strcspn(line, "\n")] = 0;
            CHECK(strchr(line, ':') && printable(strchr(line, ':') + 2));
            printf("cpu %s\n", strchr(line, ':') ? strchr(line, ':') + 2 : "?");
        }
    if (f)
        fclose(f);
    CHECK(model > 0);
    char vendor[256], board[256];
    if (read_line("/sys/class/dmi/id/board_vendor", vendor, sizeof vendor) &&
        read_line("/sys/class/dmi/id/board_name", board, sizeof board)) {
        CHECK(printable(vendor) && printable(board));
        printf("board %s %s\n", vendor, board);
    } else
        printf("board (no DMI)\n");
    printf("sysinfo failures=%d\n", failures);
    return failures != 0;
}
