/*
 * picohttpparser + protothread HTTP/1.1 server (PR-2).
 *
 * Tick-or-fd: every conn PT waits on g_chan_tick. Recv* arms WANT_READ,
 * Send arms WANT_WRITE. Drain until EAGAIN, complete, or cap — not lines.
 * Handlers are in-memory only. Chunked requests are 400 + close.
 */

#include "http_pt.h"
#include "picohttpparser.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOG_I(...) mf_log(LOG_INFO,    __VA_ARGS__)
#define LOG_W(...) mf_log(LOG_WARNING, __VA_ARGS__)
#define LOG_E(...) mf_log(LOG_ERR,     __VA_ARGS__)

#define HTTP_MAX_HEADERS 32

static pt_t http_conn_pt(env_t e_);
static pt_t http_accept_pt(env_t e_);

double mf_mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void conn_reset_req(mf_http_conn_t *c)
{
    c->last_len = 0;
    c->header_len = 0;
    c->content_length = 0;
    c->out_len = 0;
    c->out_off = 0;
    c->minor = 1;
    c->close_after = false;
    c->req_close = false;
    c->method[0] = '\0';
    c->path[0] = '\0';
    c->state = HTTP_RECV_HEADERS;
    c->select_mask = MF_IO_WANT_READ;
}

static void conn_close(mf_http_conn_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->in_use = false;
    c->select_mask = 0;
    c->state = HTTP_CLOSE;
    c->in_len = 0;
}

static bool hdr_name_eq(const struct phr_header *h, const char *name)
{
    size_t n = strlen(name);
    if (h->name == NULL || h->name_len != n)
        return false;
    return strncasecmp(h->name, name, n) == 0;
}

static bool hdr_is_identity(const struct phr_header *h)
{
    const char *v = h->value;
    size_t n = h->value_len;
    while (n > 0 && (*v == ' ' || *v == '\t')) {
        v++;
        n--;
    }
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t'))
        n--;
    return n == 8 && strncasecmp(v, "identity", 8) == 0;
}

static bool hdr_has_token(const struct phr_header *h, const char *tok)
{
    size_t tlen = strlen(tok);
    const char *v = h->value;
    size_t n = h->value_len;
    size_t i;
    for (i = 0; i + tlen <= n; i++) {
        if (i > 0) {
            char prev = v[i - 1];
            if (prev != ' ' && prev != '\t' && prev != ',')
                continue;
        }
        if (strncasecmp(v + i, tok, tlen) != 0)
            continue;
        if (i + tlen == n)
            return true;
        {
            char after = v[i + tlen];
            if (after == ',' || after == ' ' || after == '\t' || after == ';')
                return true;
        }
    }
    return false;
}

static int http_reply(mf_http_conn_t *c, int status, const char *reason,
                      const char *body, bool force_close)
{
    size_t body_len = body ? strlen(body) : 0;
    bool close_c = force_close || c->req_close || c->minor < 1;
    const char *conn = close_c ? "close" : "keep-alive";
    int n = snprintf(c->out, sizeof(c->out),
                     "HTTP/1.%d %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     c->minor >= 0 ? c->minor : 1,
                     status, reason, body_len, conn);
    if (n < 0 || (size_t)n + body_len >= sizeof(c->out))
        return -1;
    if (body_len > 0)
        memcpy(c->out + (size_t)n, body, body_len);
    c->out_len = (size_t)n + body_len;
    c->out_off = 0;
    c->close_after = close_c;
    c->state = HTTP_SEND;
    c->select_mask = MF_IO_WANT_WRITE;
    return 0;
}

static void http_reply_or_close(mf_http_conn_t *c, int status, const char *reason,
                                const char *body, bool force_close)
{
    if (http_reply(c, status, reason, body, force_close) < 0)
        conn_close(c);
}

static void copy_token(char *dst, size_t dstsz, const char *src, size_t len)
{
    size_t n = len < dstsz - 1 ? len : dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void strip_query(char *path)
{
    char *q = strchr(path, '?');
    if (q)
        *q = '\0';
}

static void handle_request(mf_http_t *h, mf_http_conn_t *c)
{
    char body[160];

    LOG_I("http %s %s %s", c->peer[0] ? c->peer : "-", c->method, c->path);

    if (strcmp(c->path, "/api/v1/health") == 0) {
        if (strcmp(c->method, "GET") != 0) {
            http_reply_or_close(c, 405, "Method Not Allowed",
                                "{\"error\":\"method not allowed\"}", false);
            return;
        }
        {
            double up = mf_mono_now() - h->start_mono;
            if (up < 0.0)
                up = 0.0;
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"server\":\"%s\",\"uptime_s\":%.3f}",
                     MF_HTTP_SERVER_ID, up);
        }
        http_reply_or_close(c, 200, "OK", body, false);
        return;
    }

    http_reply_or_close(c, 404, "Not Found", "{\"error\":\"not found\"}", false);
}

static int parse_headers(mf_http_t *h, mf_http_conn_t *c)
{
    const char *method = NULL, *path = NULL;
    size_t method_len = 0, path_len = 0;
    int minor = -1;
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t num_headers = HTTP_MAX_HEADERS;
    size_t view = c->in_len < MF_HTTP_HDR_CAP ? c->in_len : MF_HTTP_HDR_CAP;
    int pret;
    size_t i;
    bool have_cl = false;
    (void)h;

    pret = phr_parse_request(c->in, view, &method, &method_len, &path, &path_len,
                             &minor, headers, &num_headers, c->last_len);
    if (pret == -2) {
        if (c->in_len >= MF_HTTP_HDR_CAP) {
            http_reply_or_close(c, 400, "Bad Request",
                                "{\"error\":\"bad request\"}", true);
            return 1;
        }
        return 0;
    }
    if (pret == -1) {
        http_reply_or_close(c, 400, "Bad Request",
                            "{\"error\":\"bad request\"}", true);
        return 1;
    }
    if (pret < 0)
        return 0;

    c->header_len = (size_t)pret;
    c->minor = minor;
    copy_token(c->method, sizeof(c->method), method, method_len);
    copy_token(c->path, sizeof(c->path), path, path_len);
    strip_query(c->path);

    c->content_length = 0;
    c->req_close = (c->minor < 1);

    for (i = 0; i < num_headers; i++) {
        if (hdr_name_eq(&headers[i], "transfer-encoding")) {
            if (!hdr_is_identity(&headers[i])) {
                LOG_I("http %s %s %s 400 chunked",
                      c->peer[0] ? c->peer : "-", c->method, c->path);
                http_reply_or_close(c, 400, "Bad Request",
                                    "{\"error\":\"chunked not supported\"}", true);
                return 1;
            }
        } else if (hdr_name_eq(&headers[i], "content-length")) {
            char tmp[32];
            char *end = NULL;
            unsigned long long cl;
            copy_token(tmp, sizeof(tmp), headers[i].value, headers[i].value_len);
            errno = 0;
            cl = strtoull(tmp, &end, 10);
            if (errno != 0 || end == tmp) {
                http_reply_or_close(c, 400, "Bad Request",
                                    "{\"error\":\"bad request\"}", true);
                return 1;
            }
            have_cl = true;
            if (cl > MF_HTTP_BODY_CAP) {
                http_reply_or_close(c, 413, "Payload Too Large",
                                    "{\"error\":\"payload too large\"}", true);
                return 1;
            }
            c->content_length = (size_t)cl;
        } else if (hdr_name_eq(&headers[i], "connection")) {
            if (hdr_has_token(&headers[i], "close"))
                c->req_close = true;
            else if (hdr_has_token(&headers[i], "keep-alive"))
                c->req_close = false;
        }
    }
    (void)have_cl;

    if (c->content_length == 0) {
        handle_request(h, c);
        return 1;
    }
    c->state = HTTP_RECV_BODY;
    return 1;
}

static void conn_do_recv(mf_http_t *h, mf_http_conn_t *c)
{
    for (;;) {
        size_t room;
        ssize_t n;

        if (c->state == HTTP_RECV_HEADERS)
            room = (c->in_len < MF_HTTP_HDR_CAP) ? (MF_HTTP_HDR_CAP - c->in_len) : 0;
        else {
            size_t need = c->header_len + c->content_length;
            if (c->in_len >= need) {
                handle_request(h, c);
                return;
            }
            room = need - c->in_len;
            if (c->in_len + room > sizeof(c->in))
                room = sizeof(c->in) - c->in_len;
        }

        if (room == 0) {
            if (c->state == HTTP_RECV_HEADERS) {
                if (parse_headers(h, c) == 0) {
                    http_reply_or_close(c, 400, "Bad Request",
                                        "{\"error\":\"bad request\"}", true);
                }
            } else {
                http_reply_or_close(c, 400, "Bad Request",
                                    "{\"error\":\"bad request\"}", true);
            }
            return;
        }

        n = read(c->fd, c->in + c->in_len, room);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            conn_close(c);
            return;
        }
        if (n == 0) {
            conn_close(c);
            return;
        }
        c->in_len += (size_t)n;
        c->last_progress = mf_mono_now();

        if (c->state == HTTP_RECV_HEADERS) {
            int progressed = parse_headers(h, c);
            c->last_len = c->in_len < MF_HTTP_HDR_CAP ? c->in_len : MF_HTTP_HDR_CAP;
            if (c->state == HTTP_SEND || c->state == HTTP_CLOSE)
                return;
            if (progressed && c->state == HTTP_RECV_BODY)
                continue;
            /* incomplete headers; keep draining this slice */
        } else {
            if (c->in_len >= c->header_len + c->content_length) {
                handle_request(h, c);
                return;
            }
        }
    }
}

static void conn_keep_alive_reset(mf_http_t *h, mf_http_conn_t *c)
{
    size_t used = c->header_len + c->content_length;
    if (used > c->in_len)
        used = c->in_len;
    if (c->in_len > used)
        memmove(c->in, c->in + used, c->in_len - used);
    c->in_len -= used;
    conn_reset_req(c);
    if (c->in_len > 0)
        conn_do_recv(h, c);
}

static void conn_do_send(mf_http_t *h, mf_http_conn_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            conn_close(c);
            return;
        }
        if (n == 0) {
            conn_close(c);
            return;
        }
        c->out_off += (size_t)n;
        c->last_progress = mf_mono_now();
    }
    if (c->close_after) {
        conn_close(c);
        return;
    }
    conn_keep_alive_reset(h, c);
}

static void peer_name(int fd, char *dst, size_t dstsz)
{
    struct sockaddr_storage ss;
    socklen_t sl = (socklen_t)sizeof(ss);
    dst[0] = '\0';
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0)
        return;
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &in->sin_addr, dst, (socklen_t)dstsz);
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &in6->sin6_addr, dst, (socklen_t)dstsz);
    }
}

static int conn_slot(mf_http_t *h)
{
    int i;
    for (i = 0; i < MF_MAX_HTTP_CLIENTS; i++) {
        if (!h->conns[i].in_use)
            return i;
    }
    return -1;
}

static void accept_one_fd(mf_http_t *h, int fd)
{
    int slot = conn_slot(h);
    int one = 1;
    mf_http_conn_t *c;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    if (slot < 0) {
        LOG_W("http: max %d clients, dropping", MF_MAX_HTTP_CLIENTS);
        close(fd);
        return;
    }

    c = &h->conns[slot];
    memset(c, 0, sizeof(*c));
    c->in_use = true;
    c->fd = fd;
    c->last_progress = mf_mono_now();
    conn_reset_req(c);
    peer_name(fd, c->peer, sizeof(c->peer));
    c->env.srv = h;
    c->env.idx = slot;
    pt_create(h->pts, &c->thr, http_conn_pt, &c->env);
    if (h->debug)
        LOG_I("http accept fd=%d peer=%s slot=%d", fd, c->peer, slot);
}

static pt_t http_conn_pt(env_t e_)
{
    mf_http_conn_env_t *env = e_;
    mf_http_t *h = env->srv;
    mf_http_conn_t *c = &h->conns[env->idx];

    pt_resume(env);
    while (!*(h->quit) && c->in_use) {
        pt_wait(env, h->chan_tick);
        if (*(h->quit) || !c->in_use)
            break;
        if (mf_mono_now() - c->last_progress > h->idle_s) {
            if (h->debug)
                LOG_I("http idle close peer=%s", c->peer);
            conn_close(c);
            break;
        }
        if (c->state == HTTP_RECV_HEADERS || c->state == HTTP_RECV_BODY)
            conn_do_recv(h, c);
        if (c->state == HTTP_SEND)
            conn_do_send(h, c);
        if (c->state == HTTP_CLOSE || !c->in_use)
            break;
    }
    if (c->in_use)
        conn_close(c);
    return PT_DONE;
}

static void accept_drain(mf_http_t *h)
{
    for (;;) {
        int fd = accept4(h->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            LOG_W("accept: %s", strerror(errno));
            return;
        }
        accept_one_fd(h, fd);
    }
}

static pt_t http_accept_pt(env_t e_)
{
    mf_http_accept_env_t *env = e_;
    mf_http_t *h = env->srv;

    pt_resume(env);
    while (!*(h->quit)) {
        pt_wait(env, h->chan_tick);
        if (*(h->quit))
            break;
        accept_drain(h);
    }
    return PT_DONE;
}

void mf_http_init(mf_http_t *h, protothread_t pts, char *chan_tick,
                  volatile sig_atomic_t *quit, int listen_fd,
                  double idle_s, bool debug)
{
    memset(h, 0, sizeof(*h));
    h->pts = pts;
    h->chan_tick = chan_tick;
    h->quit = quit;
    h->listen_fd = listen_fd;
    h->idle_s = idle_s > 0.0 ? idle_s : MF_HTTP_IDLE_S;
    h->start_mono = mf_mono_now();
    h->debug = debug;
    h->accept_env.srv = h;
}

void mf_http_start(mf_http_t *h)
{
    int i;
    for (i = 0; i < MF_MAX_HTTP_CLIENTS; i++) {
        h->conns[i].fd = -1;
        h->conns[i].in_use = false;
    }
    pt_create(h->pts, &h->accept_thr, http_accept_pt, &h->accept_env);
}

void mf_http_prepare_fds(mf_http_t *h, fd_set *rset, fd_set *wset, int *maxfd)
{
    int i;
    if (h->listen_fd >= 0) {
        FD_SET(h->listen_fd, rset);
        if (h->listen_fd > *maxfd)
            *maxfd = h->listen_fd;
    }
    for (i = 0; i < MF_MAX_HTTP_CLIENTS; i++) {
        mf_http_conn_t *c = &h->conns[i];
        if (!c->in_use || c->fd < 0)
            continue;
        if (c->select_mask & MF_IO_WANT_READ)
            FD_SET(c->fd, rset);
        if (c->select_mask & MF_IO_WANT_WRITE)
            FD_SET(c->fd, wset);
        if ((c->select_mask & (MF_IO_WANT_READ | MF_IO_WANT_WRITE)) && c->fd > *maxfd)
            *maxfd = c->fd;
    }
}

void mf_http_close_all(mf_http_t *h)
{
    int i;
    for (i = 0; i < MF_MAX_HTTP_CLIENTS; i++) {
        if (h->conns[i].in_use || h->conns[i].fd >= 0)
            conn_close(&h->conns[i]);
    }
}
