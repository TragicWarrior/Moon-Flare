#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cli_http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_BODY (1024 * 1024)

/* Connect to the first usable address in ai, bounding the wait with a
 * non-blocking connect + select() so an unreachable host fails within
 * timeout_s instead of hanging on the kernel default. Tries each address
 * in turn. Returns a connected (blocking) fd, or -1 with *errbuf set. */
static int connect_timeout(const struct addrinfo *ai, double timeout_s,
                           char *errbuf, size_t errcap)
{
    const struct addrinfo *rp;
    int last_errno = 0;

    for (rp = ai; rp; rp = rp->ai_next)
    {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        int flags;
        int one = 1;

        if (fd < 0)
        {
            last_errno = errno;
            continue;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0)
        {
            if (errno != EINPROGRESS)
            {
                last_errno = errno;
                close(fd);
                continue;
            }
            {
                fd_set wfds;
                struct timeval tv;
                int sel;

                tv.tv_sec = (time_t)timeout_s;
                tv.tv_usec = (long)((timeout_s - (double)tv.tv_sec) * 1000000.0);
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                sel = select(fd + 1, NULL, &wfds, NULL, &tv);
                if (sel <= 0)
                {
                    last_errno = (sel == 0) ? ETIMEDOUT : errno;
                    close(fd);
                    continue;
                }
                {
                    int soerr = 0;
                    socklen_t sl = (socklen_t)sizeof(soerr);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 ||
                        soerr != 0)
                    {
                        last_errno = soerr ? soerr : errno;
                        close(fd);
                        continue;
                    }
                }
            }
        }

        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags);   /* restore blocking mode */
        return fd;
    }

    if (errbuf && errcap)
        snprintf(errbuf, errcap, "connect: %s",
                 last_errno ? strerror(last_errno) : "no usable address");
    return -1;
}

int cli_http_request(const char *host, int port, const char *method,
                     const char *path, const char *json,
                     double timeout_s, cli_http_resp_t *resp,
                     char *errbuf, size_t errcap)
{
    struct addrinfo hints, *ai;
    char portstr[16];
    char hdr[1024];
    char buf[4096];
    int fd = -1;
    size_t body_len = 0;
    char *body = NULL;
    ssize_t n;
    int rc = -1;
    struct timeval tv;
    int got_errbuf = (errbuf && errcap > 0);

    if (!host || !path || !resp)
        return -1;
    if (got_errbuf)
        errbuf[0] = '\0';

    memset(resp, 0, sizeof(*resp));

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    {
        int gai_rc = getaddrinfo(host, portstr, &hints, &ai);
        if (gai_rc != 0)
        {
            if (got_errbuf)
                snprintf(errbuf, errcap, "getaddrinfo: %s", gai_strerror(gai_rc));
            return -1;
        }
    }

    fd = connect_timeout(ai, timeout_s, got_errbuf ? errbuf : NULL, errcap);
    if (fd < 0)
        goto done;              /* errbuf set by connect_timeout; ai freed at done */

    tv.tv_sec = (time_t)timeout_s;
    tv.tv_usec = (long)((timeout_s - (double)tv.tv_sec) * 1000000.0);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Build request line + headers. */
    if (json && json[0])
    {
        (void)snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "\r\n%s",
            method ? method : "GET", path, host, strlen(json), json);
    }
    else
    {
        (void)snprintf(hdr, sizeof(hdr),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method ? method : "GET", path, host);
    }

    /* Send request. */
    {
        size_t hlen = strlen(hdr);
        size_t sent = 0;
        while (sent < hlen)
        {
            n = send(fd, hdr + sent, hlen - sent, MSG_NOSIGNAL);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (got_errbuf)
                    snprintf(errbuf, errcap, "send: %s", strerror(errno));
                goto done;
            }
            sent += (size_t)n;
        }
    }

    /* Read response headers. */
    {
        size_t hdr_end = 0;
        size_t buf_len = 0;

        for (;;)
        {
            n = recv(fd, buf + buf_len, sizeof(buf) - buf_len - 1, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    if (got_errbuf)
                        snprintf(errbuf, errcap, "recv: timeout waiting for headers");
                    goto done;
                }
                if (got_errbuf)
                    snprintf(errbuf, errcap, "recv: %s", strerror(errno));
                goto done;
            }
            if (n == 0)
                break;
            buf_len += (size_t)n;
            buf[buf_len] = '\0';

            if (buf_len >= 4)
            {
                char *sep = strstr(buf, "\r\n\r\n");
                if (sep)
                {
                    hdr_end = (size_t)(sep - buf) + 4;
                    break;
                }
            }
            if (buf_len >= MAX_BODY)
            {
                if (got_errbuf)
                    snprintf(errbuf, errcap, "response too large");
                goto done;
            }
        }

        if (hdr_end == 0)
        {
            if (got_errbuf)
                snprintf(errbuf, errcap, "no header terminator");
            goto done;
        }

        /* Parse status line from header portion. */
        {
            char status_line[256];
            size_t sl_len = hdr_end >= 4 ? hdr_end - 4 : 0;
            if (sl_len >= sizeof(status_line))
                sl_len = sizeof(status_line) - 1;
            memcpy(status_line, buf, sl_len);
            status_line[sl_len] = '\0';

            {
                char *sp1 = strchr(status_line, ' ');
                if (sp1)
                    resp->status = atoi(sp1 + 1);
                else
                    resp->status = 0;
            }

            /* Copy ONLY the body (after headers) into our buffer. */
            {
                size_t body_start = hdr_end;
                size_t remaining = buf_len - body_start;
                if (remaining > 0)
                {
                    body = (char *)malloc(remaining + 1);
                    if (!body)
                    {
                        if (got_errbuf)
                            snprintf(errbuf, errcap, "malloc");
                        goto done;
                    }
                    memcpy(body, buf + body_start, remaining);
                    body[remaining] = '\0';
                    body_len = remaining;
                }
                else
                {
                    body = (char *)malloc(1);
                    if (!body)
                    {
                        if (got_errbuf)
                            snprintf(errbuf, errcap, "malloc");
                        goto done;
                    }
                    body[0] = '\0';
                    body_len = 0;
                }
            }
        }

        /* Read remaining body to EOF. */
        for (;;)
        {
            char *new_body;
            n = recv(fd, buf, sizeof(buf), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                break;
            }
            if (n == 0)
                break;
            new_body = (char *)realloc(body, body_len + (size_t)n + 1);
            if (!new_body)
            {
                if (got_errbuf)
                    snprintf(errbuf, errcap, "realloc");
                goto done;
            }
            body = new_body;
            memcpy(body + body_len, buf, (size_t)n);
            body_len += (size_t)n;
            body[body_len] = '\0';
            if (body_len >= MAX_BODY)
            {
                if (got_errbuf)
                    snprintf(errbuf, errcap, "response too large");
                goto done;
            }
        }
    }

    resp->body = body;
    resp->body_len = body_len;
    rc = 0;

done:
    if (rc < 0)
    {
        if (resp->body)
        {
            free(resp->body);
            resp->body = NULL;
        }
        resp->body_len = 0;
        resp->status = 0;
    }
    if (got_errbuf && rc < 0 && errbuf[0] == '\0')
        snprintf(errbuf, errcap, "unknown error");
    if (fd >= 0)
        close(fd);
    freeaddrinfo(ai);
    return rc;
}

void cli_http_resp_free(cli_http_resp_t *r)
{
    if (!r)
        return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->status = 0;
}

int cli_url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p;
    size_t o = 0;

    if (!in || !out || cap == 0)
        return -1;

    for (p = (const unsigned char *)in; *p; p++)
    {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved)
        {
            if (o + 1 >= cap)
                break;
            out[o++] = (char)c;
        }
        else
        {
            if (o + 3 >= cap)
                break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
    return (int)o;
}

int cli_is_uuid(const char *s)
{
    int seen = 0;

    if (!s)
        return 0;
    for (; *s; s++)
    {
        if (*s == '-')
            continue;
        if (!isxdigit((unsigned char)*s))
            return 0;
        seen = 1;
    }
    return seen;   /* non-empty, only hex digits and dashes */
}
