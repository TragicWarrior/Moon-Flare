#include "http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECT_TMO_S 5.0
#define BACKOFF_MIN   0.5
#define BACKOFF_MAX   5.0

static void set_status(mf_http_cli_t *c, const char *s)
{
    snprintf(c->status_line, sizeof(c->status_line), "%s", s);
}

void mf_http_cli_close(mf_http_cli_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->want = 0;
    c->inflight = 0;
    c->out_len = c->out_off = 0;
    c->in_len = 0;
    c->have_body = 0;
}

void mf_http_cli_init(mf_http_cli_t *c, const char *host, int port)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->port = (port > 0 && port <= 65535) ? port : 5250;
    snprintf(c->host, sizeof(c->host), "%s", host && host[0] ? host : "127.0.0.1");
    c->state = MF_CONN_WAIT;
    c->backoff = BACKOFF_MIN;
    c->next_try = 0;
    set_status(c, "connecting");
}

static int nb_connect(mf_http_cli_t *c)
{
    struct addrinfo hints, *ai, *p;
    char portstr[16];
    int fd = -1, one = 1;

    mf_http_cli_close(c);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", c->port);
    if (getaddrinfo(c->host, portstr, &hints, &ai) != 0)
        return -1;
    for (p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    p->ai_protocol);
        if (fd < 0)
            continue;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0 || errno == EINPROGRESS)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0)
        return -1;
    c->fd = fd;
    c->state = MF_CONN_CONNECTING;
    c->want = MF_CLI_IO_WRITE;
    return 0;
}

void mf_http_cli_start(mf_http_cli_t *c, double now)
{
    if (nb_connect(c) != 0) {
        c->state = MF_CONN_WAIT;
        c->next_try = now + c->backoff;
        c->backoff *= 2.0;
        if (c->backoff > BACKOFF_MAX)
            c->backoff = BACKOFF_MAX;
        set_status(c, "reconnect wait");
        return;
    }
    c->conn_since = now;
    set_status(c, "connecting");
}

static void mark_up(mf_http_cli_t *c, double now)
{
    c->state = MF_CONN_UP;
    c->want = MF_CLI_IO_READ;
    c->backoff = BACKOFF_MIN;
    c->last_fresh = now;
    c->stale = 0;
    set_status(c, "UP");
}

static void schedule_retry(mf_http_cli_t *c, double now, const char *why)
{
    mf_http_cli_close(c);
    c->state = MF_CONN_WAIT;
    c->next_try = now + c->backoff;
    c->backoff *= 2.0;
    if (c->backoff > BACKOFF_MAX)
        c->backoff = BACKOFF_MAX;
    set_status(c, why ? why : "retry");
}

static int cli_begin(mf_http_cli_t *c, const char *method, const char *path,
                     const char *json);

static int parse_complete(mf_http_cli_t *c)
{
    char *hdr = strstr(c->in, "\r\n\r\n");
    unsigned long cl = 0;
    const char *p;
    size_t hlen;
    if (!hdr)
        return 0;
    hlen = (size_t)(hdr - c->in) + 4;
    p = strstr(c->in, "Content-Length:");
    if (p && p < hdr)
        cl = strtoul(p + 15, NULL, 10);
    if (c->in_len < hlen + (size_t)cl)
        return 0;
    if (cl >= sizeof(c->body))
        cl = sizeof(c->body) - 1;
    memcpy(c->body, c->in + hlen, (size_t)cl);
    c->body[cl] = '\0';
    c->body_len = (size_t)cl;
    c->have_body = 1;
    c->inflight = 0;
    c->in_len = 0;
    c->out_len = c->out_off = 0;
    return 1;
}

void mf_http_cli_prepare_fds(mf_http_cli_t *c, fd_set *r, fd_set *w, int *maxfd)
{
    if (c->fd < 0)
        return;
    if ((c->want & MF_CLI_IO_READ) && r)
        FD_SET(c->fd, r);
    if ((c->want & MF_CLI_IO_WRITE) && w)
        FD_SET(c->fd, w);
    if (maxfd && c->fd > *maxfd)
        *maxfd = c->fd;
}

void mf_http_cli_pump(mf_http_cli_t *c, int readable, int writable, double now)
{
    if (c->state == MF_CONN_WAIT) {
        if (now >= c->next_try)
            mf_http_cli_start(c, now);
        return;
    }
    if (c->state == MF_CONN_CONNECTING) {
        if (writable) {
            int err = 0;
            socklen_t el = (socklen_t)sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                mark_up(c, now);
            else
                schedule_retry(c, now, "connect failed");
        } else if (now - c->conn_since > CONNECT_TMO_S) {
            schedule_retry(c, now, "connect timeout");
        }
        return;
    }
    if (c->state != MF_CONN_UP)
        return;

    if (!c->inflight && c->pend) {
        char m[8], pth[160], js[2048];

        snprintf(m, sizeof(m), "%s", c->pend_method);
        snprintf(pth, sizeof(pth), "%s", c->pend_path);
        snprintf(js, sizeof(js), "%s", c->pend_json);
        c->pend = 0;
        c->pend_json[0] = '\0';
        (void)cli_begin(c, m, pth, js[0] ? js : NULL);
    }

    if (c->inflight && c->out_off < c->out_len && writable) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            schedule_retry(c, now, "send failed");
            return;
        }
        if (n > 0)
            c->out_off += (size_t)n;
        if (c->out_off >= c->out_len)
            c->want = MF_CLI_IO_READ;
    }
    if (readable) {
        ssize_t n;
        if (c->in_len >= sizeof(c->in) - 1) {
            schedule_retry(c, now, "response too large");
            return;
        }
        n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len, 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            schedule_retry(c, now, "recv failed");
            return;
        }
        if (n == 0) {
            schedule_retry(c, now, "peer closed");
            return;
        }
        if (n > 0 && c->inflight) {
            c->in_len += (size_t)n;
            c->in[c->in_len] = '\0';
            if (parse_complete(c)) {
                c->last_fresh = now;
                c->stale = 0;
                c->want = MF_CLI_IO_READ;
            }
        }
    }
}

static int cli_begin(mf_http_cli_t *c, const char *method, const char *path,
                     const char *json)
{
    if (c->state != MF_CONN_UP || c->fd < 0)
        return -1;
    if (c->inflight) {
        /* Status GET is single-flight; never clobber a queued PUT/POST. */
        if (method && strcmp(method, "GET") == 0) {
            c->skipped++;
            return 0;
        }
        snprintf(c->pend_method, sizeof(c->pend_method), "%s",
                 method ? method : "GET");
        snprintf(c->pend_path, sizeof(c->pend_path), "%s",
                 path ? path : "/");
        snprintf(c->pend_json, sizeof(c->pend_json), "%s",
                 json ? json : "");
        c->pend = 1;
        return 1;
    }
    snprintf(c->path, sizeof(c->path), "%s", path ? path : "/");
    if (json && json[0])
        c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                      "%s %s HTTP/1.1\r\nHost: %s\r\n"
                                      "Content-Type: application/json\r\n"
                                      "Content-Length: %zu\r\n"
                                      "Connection: keep-alive\r\n\r\n%s",
                                      method, c->path, c->host, strlen(json), json);
    else
        c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                      "%s %s HTTP/1.1\r\nHost: %s\r\n"
                                      "Connection: keep-alive\r\n\r\n",
                                      method, c->path, c->host);
    if (c->out_len >= sizeof(c->out))
        c->out_len = sizeof(c->out) - 1;
    c->out_off = 0;
    c->in_len = 0;
    c->have_body = 0;
    c->inflight = 1;
    c->want = MF_CLI_IO_WRITE | MF_CLI_IO_READ;
    return 1;
}

int mf_http_cli_get(mf_http_cli_t *c, const char *path)
{
    return cli_begin(c, "GET", path ? path : "/api/v1/status", NULL);
}

int mf_http_cli_post(mf_http_cli_t *c, const char *path, const char *json)
{
    return cli_begin(c, "POST", path, json);
}

int mf_http_cli_put(mf_http_cli_t *c, const char *path, const char *json)
{
    return cli_begin(c, "PUT", path, json);
}

int mf_http_cli_take_body(mf_http_cli_t *c, char *dst, size_t cap)
{
    if (!c->have_body || !dst || cap == 0)
        return 0;
    snprintf(dst, cap, "%s", c->body);
    c->have_body = 0;
    return 1;
}
