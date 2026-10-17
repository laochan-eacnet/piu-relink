#define _GNU_SOURCE
#include "config/config_internal.h"
#include "core/runtime.h"
#include "safety/safety.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char api_base[] = "http://192.168.1.42:8080/api/";
static const char cdn_base[] = "http://10.20.30.40:8090/content";
static struct sockaddr_in connected;
static unsigned connections;
static char rewritten[2048];

void pt_log(const char *component, const char *format, ...)
{
    (void)component;
    (void)format;
}

static int record_setopt(void *handle, int option, ...)
{
    va_list ap;
    const char *url;
    (void)handle;
    assert(option == CURLOPT_URL_VALUE);
    va_start(ap, option);
    url = va_arg(ap, const char *);
    va_end(ap);
    assert(strlen(url) < sizeof(rewritten));
    strcpy(rewritten, url);
    return 0;
}

int (*pt_real_curl_easy_setopt)(void *, int, ...) = record_setopt;
int curl_easy_setopt(void *handle, int option, ...);

static int record_connect(int fd, const struct sockaddr *address, socklen_t size)
{
    (void)fd;
    assert(size == sizeof(connected));
    connected = *(const struct sockaddr_in *)address;
    ++connections;
    return 0;
}

/* The policy is linked with dlsym renamed: no real destination is contacted. */
void *network_test_dlsym(void *handle, const char *name)
{
    (void)handle;
    if (!strcmp(name, "fopen"))
        return (void *)fopen;
    return !strcmp(name, "connect") ? (void *)record_connect : NULL;
}

static void load_config(const char *api, const char *cdn, const char *port)
{
    char path[] = "/tmp/phoenixtools-network-XXXXXX";
    int fd = mkstemp(path);
    FILE *file;

    assert(fd >= 0);
    file = fdopen(fd, "w");
    assert(file);
    assert(fprintf(file, "[server]\napi_base=%s\ncdn_base=%s\n", api, cdn) > 0);
    if (port)
        assert(fprintf(file, "game_port=%s\n", port) > 0);
    assert(fclose(file) == 0);
    pt_config_load_path(path);
    assert(!strcmp(pt_config_get()->loaded_from, path));
    assert(unlink(path) == 0);
}

static struct sockaddr_in destination(const char *address, unsigned port)
{
    struct sockaddr_in result = {.sin_family = AF_INET, .sin_port = htons(port)};
    assert(inet_pton(AF_INET, address, &result.sin_addr) == 1);
    return result;
}

static void expect_connect(const char *original, unsigned port,
                           const char *expected, unsigned expected_port)
{
    struct sockaddr_in target = destination(original, port);
    struct sockaddr_in wanted = destination(expected, expected_port);
    unsigned before = connections;
    assert(connect(-1, (struct sockaddr *)&target, sizeof(target)) == 0);
    assert(connections == before + 1);
    assert(connected.sin_addr.s_addr == wanted.sin_addr.s_addr);
    assert(connected.sin_port == wanted.sin_port);
}

static void expect_blocked(const char *address, unsigned port, int error)
{
    struct sockaddr_in target = destination(address, port);
    unsigned before = connections;
    assert(connect(-1, (struct sockaddr *)&target, sizeof(target)) == -1);
    assert(errno == error && connections == before);
}

static void expect_dns(const char *expected)
{
    struct hostent *host = gethostbyname("primedaemon.piugame.com");
    struct sockaddr_in wanted = destination(expected, 0);
    assert(host && host->h_addrtype == AF_INET && host->h_length == 4);
    assert(!memcmp(host->h_addr_list[0], &wanted.sin_addr, 4));
}

static void expect_url(const char *original, const char *expected)
{
    assert(curl_easy_setopt(NULL, CURLOPT_URL_VALUE, original) == 0);
    assert(!strcmp(rewritten, expected));
}

static void test_game_ports(void)
{
    static const struct {
        const char *value;
        unsigned int port;
    } valid[] = {
        {NULL, 15040}, {"16543", 16543}, {"1", 1}, {"65535", 65535}
    };
    static const char *const invalid[] = {
        "", "0", "65536", "-1", "not-a-port", "16543junk", "0x409f",
        "999999999999999999999999999999"
    };
    size_t index;

    for (index = 0; index < sizeof(valid) / sizeof(valid[0]); ++index) {
        load_config(api_base, cdn_base, valid[index].value);
        assert(pt_config_get()->game_port == valid[index].port);
        expect_connect("198.51.100.9", 50400, "192.168.1.42", valid[index].port);
    }
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        load_config(api_base, cdn_base, invalid[index]);
        assert(pt_config_get()->game_port == 15040);
        expect_connect("198.51.100.9", 50400, "192.168.1.42", 15040);
    }
}

int main(void)
{
    load_config("", "", NULL);
    assert(pt_config_get()->game_port == 15040);
    assert(gethostbyname("primedaemon.piugame.com") == NULL);
    expect_blocked("198.51.100.9", 50400, EACCES);
    expect_blocked("127.0.0.1", 8080, EACCES);
    expect_url("https://www.piugame.com/piu.module/login.php", "offline://blocked");

    load_config(api_base, cdn_base, NULL);
    assert(pt_network_private_base(pt_config_get()->api_base));
    expect_dns("192.168.1.42");
    expect_connect("192.168.1.42", 8080, "192.168.1.42", 8080);
    expect_connect("10.20.30.40", 8090, "10.20.30.40", 8090);
    expect_blocked("192.168.1.42", 8081, EACCES);
    expect_blocked("192.168.1.43", 8080, EACCES);
    expect_blocked("198.51.100.9", 80, EACCES);
    expect_connect("198.51.100.9", 50400, "192.168.1.42", 15040);
    test_game_ports();

    expect_url("https://www.piugame.com/piu.module/login.php?x=1",
               "http://192.168.1.42:8080/api/piu.module/login.php?x=1");
    expect_url("http://xx-cdn.piugame.com/2023/ad/banner.png",
               "http://10.20.30.40:8090/content/2023/ad/banner.png");
    expect_url("http://xx-cdn.piugame.com:80/2023/ad/banner.png",
               "http://10.20.30.40:8090/content/2023/ad/banner.png");
    expect_url("http://xx-cdn.piugame.com.example/2023/ad/banner.png",
               "http://192.168.1.42:8080/api/2023/ad/banner.png");
    expect_url("http://www.piugame.com/xx-cdn.piugame.com/2023/ad/banner.png",
               "http://192.168.1.42:8080/api/xx-cdn.piugame.com/2023/ad/banner.png");

    load_config("", cdn_base, NULL);
    assert(!pt_network_private_base(pt_config_get()->api_base));
    expect_dns("10.20.30.40");
    expect_connect("10.20.30.40", 8090, "10.20.30.40", 8090);
    expect_blocked("198.51.100.9", 50400, EACCES);
    expect_url("http://www2.piugame.com/ip/", "offline://blocked");
    load_config("http://198.51.100.9:8080", cdn_base, NULL);
    assert(!pt_network_private_base(pt_config_get()->api_base));
    expect_blocked("198.51.100.9", 50400, EACCES);
    load_config("http://198.51.100.9:8080", "http://192.168.1.999:8090", NULL);
    expect_url("http://xx-cdn.piugame.com/2023/ad/banner.png", "offline://blocked");
    assert(gethostbyname("primedaemon.piugame.com") == NULL);

    load_config("http://localhost:8080", "", NULL);
    expect_dns("127.0.0.1");
    expect_connect("198.51.100.9", 50400, "127.0.0.1", 15040);
    expect_connect("127.0.0.1", 8080, "127.0.0.1", 8080);
    puts("configured private network routing: PASS");
    return 0;
}
