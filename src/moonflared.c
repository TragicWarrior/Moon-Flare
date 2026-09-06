/*
 * moonflared -- Moon Flare collector daemon.
 *
 * Phase-1 skeleton (PR-1): bind --listen, 50 ms select() tick, one
 * idle accept protothread. HTTP is PR-2; config load is PR-3; plugins
 * are PR-4. Default bind 0.0.0.0:5250 (not 5252/5253).
 *
 * Concurrency: Larry Ruane protothreads (protothread.h) + select().
 * Device step() / HTTP conn PTs (later) run on fd-ready or tick;
 * always pt_wait on g_chan_tick. Listen fd is O_NONBLOCK.
 *
 * CLI:
 *   moonflared [--listen [HOST:]PORT] [--foreground] [--config PATH]
 *              [--help]
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* daemon() */

#include "protothread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define DEFAULT_LISTEN "0.0.0.0:5250"
#define TICK_US        50000          /* 50 ms */

static bool g_foreground = false;
static bool g_debug      = false;

static void daemon_log(int prio, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_foreground) {
        fprintf(stderr, "%s\n", buf);
    } else {
        syslog(prio, "%s", buf);
    }
}

#define LOG_I(...) daemon_log(LOG_INFO,    __VA_ARGS__)
#define LOG_W(...) daemon_log(LOG_WARNING, __VA_ARGS__)
#define LOG_E(...) daemon_log(LOG_ERR,     __VA_ARGS__)

static state_t               g_pts;
static int                   g_listen_fd = -1;
static char                  g_chan_tick;
static volatile sig_atomic_t g_quit = 0;

static void on_quit(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ------------------------------------------------------------------ */
/* TCP listener                                                       */
/* ------------------------------------------------------------------ */

static int parse_listen(const char *spec, char *host, size_t hostsz, int *port)
{
    const char *colon = strrchr(spec, ':');
    if (!colon) {
        int n = snprintf(host, hostsz, "0.0.0.0");
        if (n < 0 || (size_t)n >= hostsz) return -1;
        *port = atoi(spec);
        return 0;
    }
    size_t hlen = (size_t)(colon - spec);
    if (hlen == 0 || hlen >= hostsz) return -1;
    memcpy(host, spec, hlen);
    host[hlen] = '\0';
    *port = atoi(colon + 1);
    return 0;
}

static int listen_tcp(const char *spec)
{
    char host[128];
    int port = 0;
    if (parse_listen(spec, host, sizeof(host), &port) < 0 || port <= 0 || port > 65535) {
        LOG_E("invalid --listen: %s", spec);
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    char portstr[16];
    int n = snprintf(portstr, sizeof(portstr), "%d", port);
    if (n < 0 || (size_t)n >= sizeof(portstr)) return -1;

    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(host[0] ? host : NULL, portstr, &hints, &ai);
    if (rc != 0) {
        LOG_E("getaddrinfo(%s,%d): %s", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK, p->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) {
            if (listen(fd, 16) == 0) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        LOG_E("listen on %s:%d failed: %s", host, port, strerror(errno));
        return -1;
    }
    LOG_I("listening on %s:%d", host[0] ? host : "*", port);
    return fd;
}

/* Drain the accept queue. HTTP request handling is PR-2; until then
 * drop the connection so the backlog cannot fill. */
static void accept_drain(void)
{
    for (;;) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            LOG_W("accept: %s", strerror(errno));
            return;
        }
        if (g_debug)
            LOG_I("accept fd=%d (closed; HTTP is PR-2)", cfd);
        close(cfd);
    }
}

typedef struct {
    pt_func_t pt_func;
} accept_env_t;

static accept_env_t g_accept_env;
static pt_thread_t  g_accept_thr;

static pt_t accept_pt(env_t e_)
{
    accept_env_t *env = e_;
    pt_resume(env);
    while (!g_quit) {
        pt_wait(env, &g_chan_tick);
        if (g_quit) break;
        accept_drain();
    }
    return PT_DONE;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "moonflared -- Moon Flare collector daemon\n"
        "\n"
        "Usage: %s [--listen [HOST:]PORT] [--foreground] [--config PATH]\n"
        "          [--debug] [--help]\n"
        "\n"
        "  --listen     bind address (default %s)\n"
        "  --foreground do not daemonize; log to stderr\n"
        "  --config     path to moonflared.json (load is PR-3; accepted now)\n"
        "  --debug      extra accept logging\n"
        "\n"
        "Does not bind 5252 or 5253 (xd_bmsd / jkbmsd soak ports).\n"
        "Example:\n"
        "  %s --listen 127.0.0.1:5250 --foreground\n",
        prog, DEFAULT_LISTEN, prog);
}

int main(int argc, char **argv)
{
    const char *listen_spec = DEFAULT_LISTEN;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (!strcmp(argv[i], "--foreground")) {
            g_foreground = true;
        } else if (!strcmp(argv[i], "--debug")) {
            g_debug = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!g_foreground)
        openlog("moonflared", LOG_PID, LOG_DAEMON);

    if (config_path)
        LOG_I("config path %s (load is PR-3; using built-in listen)", config_path);

    g_listen_fd = listen_tcp(listen_spec);
    if (g_listen_fd < 0)
        return 1;

    if (!g_foreground) {
        if (daemon(0, 0) < 0) {
            LOG_E("daemon(): %s", strerror(errno));
            return 1;
        }
    }

    signal(SIGINT,  on_quit);
    signal(SIGTERM, on_quit);
    signal(SIGPIPE, SIG_IGN);

    g_pts = protothread_create();
    if (!g_pts) {
        LOG_E("protothread_create failed");
        close(g_listen_fd);
        return 1;
    }
    pt_create(g_pts, &g_accept_thr, accept_pt, &g_accept_env);
    while (protothread_run(g_pts))
        ; /* park accept_pt on g_chan_tick before the first select */

    while (!g_quit) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(g_listen_fd, &rset);
        int maxfd = g_listen_fd;

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = (suseconds_t)TICK_US;

        int n = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (n < 0 && errno != EINTR) {
            LOG_E("select: %s", strerror(errno));
            break;
        }

        /* Tick-or-fd: always broadcast the 50 ms tick. Listen-ready
         * also wakes the same channel (KD 10). */
        pt_broadcast(g_pts, &g_chan_tick);
        while (protothread_run(g_pts))
            ;
    }

    LOG_I("shutting down");
    g_quit = 1;
    pt_broadcast(g_pts, &g_chan_tick);
    while (protothread_run(g_pts))
        ;
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    protothread_free(g_pts);
    if (!g_foreground)
        closelog();
    return 0;
}
