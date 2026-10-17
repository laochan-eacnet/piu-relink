/* The easy API used by Phoenix, restricted to explicit private IPv4 HTTP.
 * No TLS, proxies, redirects, DNS or public hosts are supported. */
#include "safety/private_http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct easy {
    char *url;
    const char *post;
    long post_size, timeout, response, post_enabled, no_progress;
    size_t (*write)(char *, size_t, size_t, void *);
    int (*progress)(void *, double, double, double, double);
    void *data, *progress_data;
    double received, elapsed;
};

void *curl_easy_init(void)
{
    struct easy *e = calloc(1, sizeof(*e));
    if (e) {
        e->post_size = -1;
        e->timeout = 3;
        e->no_progress = 1;
    }
    return e;
}

void curl_easy_cleanup(void *handle)
{
    struct easy *e = handle;
    if (e) { free(e->url); free(e); }
}

int curl_easy_setopt(void *handle, int option, ...)
{
    struct easy *e = handle;
    va_list ap;
    uintptr_t v;
    if (!e) return 2;
    va_start(ap, option);
    if (option >= 30000 && option < 40000) {
        (void)va_arg(ap, long long); /* MAX_RECV_SPEED_LARGE: local only. */
        va_end(ap);
        return option == 30146 ? 0 : 48;
    }
    v = va_arg(ap, uintptr_t);
    va_end(ap);
    switch (option) {
    case 10002: {
        char *url = v ? strdup((const char *)v) : NULL;
        if (v && !url) return 27;
        free(e->url); e->url = url; break;
    }
    case 10001: e->data = (void *)v; break;
    case 20011: e->write = (void *)v; break;
    case 20056: e->progress = (void *)v; break;
    case 10057: e->progress_data = (void *)v; break;
    case 10015: e->post = (const char *)v; e->post_enabled = 1; break;
    case 60: e->post_size = (long)v; break;
    case 47: e->post_enabled = (long)v; break;
    case 80: if (v) e->post_enabled = 0; break;
    case 13: case 78: e->timeout = (long)v > 0 ? (long)v : 3; break;
    case 43: e->no_progress = (long)v; break;
    case 21: case 64: case 99: break; /* RESUME_FROM, TLS verify, NOSIGNAL. */
    default: return 48;
    }
    return 0;
}

int curl_easy_getinfo(void *handle, int info, ...)
{
    struct easy *e = handle;
    va_list ap;
    void *out;
    if (!e) return 2;
    va_start(ap, info); out = va_arg(ap, void *); va_end(ap);
    if (!out) return 43;
    switch (info) {
    case 0x200002: *(long *)out = e->response; break;
    case 0x300003: *(double *)out = e->elapsed; break;
    case 0x300008: *(double *)out = e->received; break;
    case 0x300009: *(double *)out = e->elapsed ? e->received/e->elapsed : 0; break;
    default: return 48;
    }
    return 0;
}


static int send_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 55;
        data += n; length -= (size_t)n;
    }
    return 0;
}

static int deliver(struct easy *e, char *data, size_t length)
{
    size_t written = e->write ? e->write(data, 1, length, e->data) :
                    fwrite(data, 1, length, e->data ? e->data : stdout);
    if (written != length) return 23;
    e->received += length;
    if (!e->no_progress && e->progress &&
        e->progress(e->progress_data, 0, e->received, 0, 0)) return 42;
    return 0;
}

static int receive_response(int fd, struct easy *e)
{
    char buffer[16384];
    size_t used = 0;
    int headers = 1;
    long long expected = -1;
    for (;;) {
        ssize_t n = recv(fd, buffer+used, sizeof(buffer)-used-1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 28 : 56;
        if (!n) return headers || (expected >= 0 && e->received != expected) ? 18 : 0;
        used += (size_t)n;
        buffer[used] = 0;
        if (headers) {
            char *body = strstr(buffer, "\r\n\r\n"), *line;
            size_t header_length;
            int status;
            if (!body) {
                if (used == sizeof(buffer)-1) return 56;
                continue;
            }
            if (sscanf(buffer, "HTTP/%*u.%*u %d", &status) != 1) return 56;
            e->response = status;
            header_length = (size_t)(body+4-buffer);
            for (line = strstr(buffer, "\r\n"); line && line < body;
                 line = strstr(line+2, "\r\n")) {
                if (!strncasecmp(line+2, "Transfer-Encoding:", 18)) return 1;
                if (!strncasecmp(line+2, "Content-Length:", 15))
                    expected = strtoll(line+17, NULL, 10);
            }
            headers = 0;
            used -= header_length;
            if (used) {
                int result = deliver(e, buffer+header_length, used);
                if (result) return result;
            }
        } else {
            int result = deliver(e, buffer, used);
            if (result) return result;
        }
        used = 0;
        if (expected >= 0 && e->received >= expected)
            return e->received == expected ? 0 : 18;
    }
}

int curl_easy_perform(void *handle)
{
    struct easy *e = handle;
    struct pt_private_http request;
    struct timeval timeout, start, finish;
    char header[4096];
    size_t body_size;
    int fd, result, length;
    if (!e) return 2;
    e->response = 0; e->received = 0; e->elapsed = 0;
    result = pt_private_http_parse(e->url, &request);
    if (result) return result;
    body_size = e->post_enabled && e->post ?
                (e->post_size < 0 ? strlen(e->post) : (size_t)e->post_size) : 0;
    length = snprintf(header, sizeof(header),
        "%s %s HTTP/1.0\r\nHost: %.*s:%u\r\nConnection: close\r\n"
        "Content-Length: %zu\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n",
        e->post_enabled ? "POST" : "GET", request.path,
        (int)request.host_length, request.host, ntohs(request.address.sin_port), body_size);
    if (length < 0 || (size_t)length >= sizeof(header)) return 3;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 7;
    timeout.tv_sec = e->timeout; timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    gettimeofday(&start, NULL);
    result = connect(fd, (struct sockaddr *)&request.address, sizeof(request.address)) < 0 ? 7 : 0;
    if (!result) result = send_all(fd, header, (size_t)length);
    if (!result && body_size) result = send_all(fd, e->post, body_size);
    if (!result) result = receive_response(fd, e);
    close(fd);
    gettimeofday(&finish, NULL);
    e->elapsed = finish.tv_sec-start.tv_sec + (finish.tv_usec-start.tv_usec)/1000000.0;
    return result;
}

const char *curl_easy_strerror(int code)
{
    switch (code) {
    case 0: return "No error";
    case 1: return "Unsupported protocol (only private IPv4 HTTP is enabled)";
    case 3: return "Malformed URL";
    case 7: return "Connection blocked or refused";
    case 18: return "Partial response";
    case 23: return "Write callback failed";
    case 27: return "Out of memory";
    case 28: return "Operation timed out";
    case 42: return "Aborted by progress callback";
    case 48: return "Unsupported easy option";
    case 55: return "Send failed";
    case 56: return "Invalid HTTP response or receive failed";
    default: return "Invalid easy handle or argument";
    }
}
