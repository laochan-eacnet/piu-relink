#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>

static void check_private_bases(int (*private_base)(const char *))
{
    static const char *accepted[] = {
        "http://127.0.0.1:8080", "http://localhost:8080/isolated",
        "http://10.0.0.0", "http://10.255.255.255:65535/",
        "http://172.16.0.0", "http://172.31.255.255/prefix",
        "http://192.168.0.0:1", "http://192.168.255.255:8080/"
    };
    static const char *rejected[] = {
        NULL, "", "https://127.0.0.1", "http://localhost.example",
        "http://127.0.0.10", "http://127.1", "http://2130706433",
        "http://0x7f000001", "http://[::1]", "http://169.254.1.1",
        "http://9.255.255.255", "http://11.0.0.0", "http://172.15.255.255",
        "http://172.32.0.0", "http://192.167.255.255", "http://192.169.0.0",
        "http://100.64.0.1", "http://198.51.100.9", "http://www.piugame.com",
        "http://192.168.1.256", "http://192.168.1", "http://192.168.001.2",
        "http://192.168.1.2.", "http://192.168.1.2.example",
        "http://192.168.1.2:0", "http://192.168.1.2:65536",
        "http://192.168.1.2:99999999999999999999999999999999",
        "http://192.168.1.2:+80", "http://192.168.1.2:",
        "http://user@192.168.1.2", "http://192.168.1.2:80@remote",
        "http://192.168.1.2/path\r\nHost: remote",
        "http://192.168.1.2/path?query", "http://192.168.1.2/path#fragment"
    };
    size_t i;
    for (i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i)
        assert(private_base(accepted[i]));
    for (i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i)
        assert(!private_base(rejected[i]));
}

int main(int argc, char **argv)
{
    void *hook, *curl, *easy;
    void *(*init)(void);
    void (*cleanup)(void *);
    int (*setopt)(void *, int, ...), (*perform)(void *), (*getinfo)(void *, int, ...);
    int (*policy_connect)(int, const struct sockaddr *, socklen_t);
    struct hostent *(*policy_dns)(const char *);
    int (*private_base)(const char *);
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(50400)};
    long status = 123;
    assert(argc == 3);
    hook = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    assert(hook);
    curl = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    assert(curl);
    init = dlsym(hook, "curl_easy_init");
    cleanup = dlsym(hook, "curl_easy_cleanup");
    setopt = dlsym(hook, "curl_easy_setopt");
    perform = dlsym(hook, "curl_easy_perform");
    getinfo = dlsym(hook, "curl_easy_getinfo");
    policy_connect = dlsym(hook, "connect");
    policy_dns = dlsym(hook, "gethostbyname");
    private_base = dlsym(hook, "pt_network_private_base");
    assert(private_base);
    check_private_bases(private_base);
    assert(init && cleanup && setopt && perform && getinfo && policy_connect && policy_dns);
    easy = init(); assert(easy);
    assert(setopt(easy, 10002, "https://www.piugame.com/piu.module/piu.get_ip_to_country.php") == 0);
    assert(perform(easy) != 0);
    assert(getinfo(easy, 0x200002, &status) == 0 && status == 0);
    assert(policy_dns("primedaemon.piugame.com") == NULL);
    inet_pton(AF_INET, "198.51.100.9", &remote.sin_addr);
    assert(policy_connect(-1, (struct sockaddr *)&remote, sizeof(remote)) == -1 && errno == EACCES);
    cleanup(easy);
    /* The transport itself must reject real hosts/TLS even without the hook. */
    init = dlsym(curl, "curl_easy_init");
    cleanup = dlsym(curl, "curl_easy_cleanup");
    setopt = dlsym(curl, "curl_easy_setopt");
    perform = dlsym(curl, "curl_easy_perform");
    easy = init(); assert(easy);
    assert(setopt(easy, 10002, "http://198.51.100.9/") == 0 && perform(easy) == 7);
    assert(setopt(easy, 10002, "https://127.0.0.1/") == 0 && perform(easy) == 1);
    cleanup(easy);
    puts("network isolation: PASS");
    return 0;
}
