/*
 * PR-2 merge bar: health 200 + Content-Length, partial-hold peer vs
 * peer B, idle close via --http-idle-s=0.2, chunked → 400.
 *
 * Spawns moonflared --foreground on an ephemeral 127.0.0.1 port.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } while (0)

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_s(double s)
{
    struct timespec ts;
    time_t sec = (time_t)s;
    long nsec = (long)((s - (double)sec) * 1e9);
    if (nsec < 0)
        nsec = 0;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    nanosleep(&ts, NULL);
}

static int pick_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t sl = (socklen_t)sizeof(a);
    int one = 1;
    if (s < 0)
        return -1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        close(s);
        return -1;
    }
    if (getsockname(s, (struct sockaddr *)&a, &sl) != 0)
    {
        close(s);
        return -1;
    }
    close(s);
    return (int)ntohs(a.sin_port);
}

static int tcp_connect(int port, double timeout_s)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    int one = 1;
    struct pollfd pfd;
    int flags;
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS)
    {
        close(fd);
        return -1;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, (int)(timeout_s * 1000.0)) <= 0)
    {
        close(fd);
        return -1;
    }
    {
        int err = 0;
        socklen_t el = (socklen_t)sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0)
        {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int send_all(int fd, const char *s)
{
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, s + off, len - off);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, 1000) <= 0)
                    return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Read until headers + Content-Length body, or timeout. */
static int recv_http(int fd, char *buf, size_t cap, size_t *out, double timeout_s)
{
    double deadline = mono_now() + timeout_s;
    *out = 0;
    while (*out + 1 < cap)
    {
        double left = deadline - mono_now();
        struct pollfd pfd;
        ssize_t n;
        char *hdr_end;
        if (left <= 0.0)
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)(left * 1000.0 + 0.5)) <= 0)
            return -1;
        n = read(fd, buf + *out, cap - 1 - *out);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        *out += (size_t)n;
        buf[*out] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end)
        {
            size_t header_bytes = (size_t)(hdr_end - buf) + 4;
            const char *cl = strstr(buf, "Content-Length:");
            unsigned long body_len = 0;
            if (!cl)
                cl = strstr(buf, "content-length:");
            if (cl)
                body_len = strtoul(cl + 15, NULL, 10);
            if (*out >= header_bytes + (size_t)body_len)
                return 0;
        }
    }
    buf[*out] = '\0';
    return 0;
}

static int status_of(const char *resp)
{
    int st = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &st) != 1)
        return -1;
    return st;
}

static int has_content_length(const char *resp)
{
    return strstr(resp, "Content-Length:") != NULL
        || strstr(resp, "content-length:") != NULL;
}

static pid_t spawn_daemon(const char *bin, int port, int logfd)
{
    pid_t pid = fork();
    char spec[64];
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        if (logfd >= 0)
        {
            dup2(logfd, STDERR_FILENO);
            if (logfd != STDERR_FILENO)
                close(logfd);
        }
        snprintf(spec, sizeof(spec), "127.0.0.1:%d", port);
        execl(bin, bin, "--listen", spec, "--foreground",
              "--http-idle-s", "0.2", "--debug", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop_daemon(pid_t pid)
{
    int i;
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    for (i = 0; i < 40; i++)
    {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        sleep_s(0.05);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int main(int argc, char **argv)
{
    const char *bin;
    int port;
    int logfd;
    pid_t pid;
    int i;
    int ready = 0;
    char resp[2048];
    size_t nresp;
    int fd, a, b;
    double t0, dt;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s /path/to/moonflared\n", argv[0]);
        return 2;
    }
    bin = argv[1];
    port = pick_port();
    if (port <= 0)
    {
        FAIL("pick_port");
        return 1;
    }

    logfd = open("/tmp/mf-http-core.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid = spawn_daemon(bin, port, logfd);
    if (logfd >= 0)
        close(logfd);
    if (pid < 0)
    {
        FAIL("fork moonflared");
        return 1;
    }

    for (i = 0; i < 50; i++)
    {
        fd = tcp_connect(port, 0.1);
        if (fd >= 0)
        {
            close(fd);
            ready = 1;
            break;
        }
        sleep_s(0.05);
    }
    if (!ready)
    {
        FAIL("daemon did not listen");
        stop_daemon(pid);
        return 1;
    }

    /* 1. GET /api/v1/health → 200 JSON with Content-Length */
    fd = tcp_connect(port, 1.0);
    if (fd < 0)
    {
        FAIL("connect health");
    }
    else
    {
        const char *req =
            "GET /api/v1/health HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n";
        if (send_all(fd, req) < 0)
            FAIL("send health");
        else if (recv_http(fd, resp, sizeof(resp), &nresp, 1.0) < 0)
            FAIL("recv health timeout");
        else if (status_of(resp) != 200)
            FAIL("health status != 200");
        else if (!has_content_length(resp))
            FAIL("health missing Content-Length");
        else if (!strstr(resp, "\"ok\":true"))
            FAIL("health body missing ok");
        else if (!strstr(resp, "moonflared/0.1.0"))
            FAIL("health body missing server id");
        close(fd);
    }

    /* 2. Peer A partial hold; peer B health within ~100 ms */
    a = tcp_connect(port, 1.0);
    if (a < 0)
    {
        FAIL("connect peer A");
    } else if (send_all(a, "GET /api/v1/hea") < 0)
    {
        FAIL("send partial A");
        close(a);
        a = -1;
    }

    t0 = mono_now();
    b = tcp_connect(port, 1.0);
    if (b < 0)
    {
        FAIL("connect peer B");
    }
    else
    {
        const char *req =
            "GET /api/v1/health HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n";
        if (send_all(b, req) < 0)
            FAIL("send B");
        else if (recv_http(b, resp, sizeof(resp), &nresp, 1.0) < 0)
            FAIL("recv B timeout");
        else if (status_of(resp) != 200)
            FAIL("peer B status != 200");
        dt = mono_now() - t0;
        if (dt > 0.20)
            FAIL("peer B slower than 200 ms");
        close(b);
    }

    /* 3. Idle close of A at 0.2 s */
    if (a >= 0)
    {
        sleep_s(0.45);
        {
            char tmp[8];
            struct pollfd pfd = { .fd = a, .events = POLLIN };
            int pr = poll(&pfd, 1, 200);
            ssize_t nr;
            if (pr < 0)
                FAIL("poll A after idle");
            nr = read(a, tmp, sizeof(tmp));
            if (nr > 0)
                FAIL("peer A still has data after idle");
            else if (nr < 0 && errno != EAGAIN && errno != ECONNRESET)
                FAIL("peer A unexpected errno after idle");
            /* nr==0 (EOF) or ECONNRESET: closed. EAGAIN: not yet — fail */
            if (nr < 0 && errno == EAGAIN)
                FAIL("peer A still open after idle");
        }
        close(a);
    }

    /* 4. Chunked request → 400 */
    fd = tcp_connect(port, 1.0);
    if (fd < 0)
    {
        FAIL("connect chunked");
    }
    else
    {
        const char *req =
            "GET /api/v1/health HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n";
        if (send_all(fd, req) < 0)
            FAIL("send chunked");
        else if (recv_http(fd, resp, sizeof(resp), &nresp, 1.0) < 0)
            FAIL("recv chunked timeout");
        else if (status_of(resp) != 400)
            FAIL("chunked status != 400");
        else if (!strstr(resp, "chunked not supported"))
            FAIL("chunked body missing error");
        else if (!has_content_length(resp))
            FAIL("chunked missing Content-Length");
        close(fd);
    }

    stop_daemon(pid);
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed (daemon log /tmp/mf-http-core.log)\n", g_fail);
        return 1;
    }
    printf("http_core: ok (port %d)\n", port);
    return 0;
}
