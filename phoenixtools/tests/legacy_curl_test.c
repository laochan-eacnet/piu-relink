#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>

void *curl_easy_init(void);
void curl_easy_cleanup(void *handle);
int curl_easy_setopt(void *handle, int option, ...);
int curl_easy_perform(void *handle);

static struct sockaddr_in connected;
static unsigned connections;

/* Only connect is replaced; the transport still creates/closes its own socket. */
int legacy_test_connect(int fd, const struct sockaddr *address, socklen_t size)
{
    (void)fd;
    assert(size == sizeof(connected));
    connected = *(const struct sockaddr_in *)address;
    ++connections;
    errno = ECONNREFUSED;
    return -1;
}

int main(void)
{
    static const struct {
        const char *url, *address;
        unsigned port;
    } accepted[] = {
        {"http://127.0.0.1:8080/", "127.0.0.1", 8080},
        {"http://localhost:8080/", "127.0.0.1", 8080},
        {"http://192.168.1.42:8080/ip/?x=1", "192.168.1.42", 8080},
        {"http://10.20.30.40/2023/ad/banner.png", "10.20.30.40", 80},
        {"http://172.16.0.1:1234/", "172.16.0.1", 1234},
        {"http://172.31.255.254:65535/", "172.31.255.254", 65535}
    };
    static const char *rejected[] = {
        "http://198.51.100.9/", "http://www.piugame.com/",
        "http://172.15.255.255/", "http://172.32.0.0/",
        "http://192.169.0.1/", "http://192.168.1.256/",
        "http://192.168.1.2.example/", "http://192.168.001.2/",
        "http://192.168.1.2:80@remote/", "http://192.168.1.2:65536/",
        "http://192.168.1.2:80/path\r\nHost: remote",
        "https://192.168.1.2/", "offline://blocked"
    };
    void *easy = curl_easy_init();
    size_t i;
    assert(easy);
    for (i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
        struct in_addr expected;
        unsigned before = connections;
        assert(inet_pton(AF_INET, accepted[i].address, &expected) == 1);
        assert(curl_easy_setopt(easy, 10002, accepted[i].url) == 0);
        assert(curl_easy_perform(easy) == 7);
        assert(connections == before + 1);
        assert(connected.sin_family == AF_INET);
        assert(connected.sin_addr.s_addr == expected.s_addr);
        assert(ntohs(connected.sin_port) == accepted[i].port);
    }
    for (i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        unsigned before = connections;
        assert(curl_easy_setopt(easy, 10002, rejected[i]) == 0);
        assert(curl_easy_perform(easy) != 0);
        assert(connections == before);
    }
    curl_easy_cleanup(easy);
    puts("legacy private HTTP destinations: PASS");
    return 0;
}
