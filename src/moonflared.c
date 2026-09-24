/*
 * moonflared -- Moon Flare collector daemon.
 *
 * PR-2: bind --listen, 50 ms select() tick, picohttpparser HTTP on
 * accepted fds. GET /api/v1/health is in-memory. PUT /config applies a
 * slot diff (KD 29); startup opens devices[]. Default bind 0.0.0.0:5250
 * (not 5252/5253).
 *
 * Concurrency: Larry Ruane protothreads (protothread.h) + select().
 * HTTP conn PTs run on fd-ready or tick; always pt_wait on g_chan_tick.
 * Listen and accepted fds are O_NONBLOCK.
 *
 * CLI:
 *   moonflared [--listen [HOST:]PORT] [--foreground] [--config PATH]
 *              [--http-idle-s SEC] [--debug] [--help]
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* daemon() */

#include "config.h"
#include "device.h"
#include "discover.h"
#include "history.h"
#include "http_pt.h"
#include "loader.h"
#include "protothread.h"
#include "rest.h"

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
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LISTEN "0.0.0.0:5250"
#define TICK_US        50000          /* 50 ms */

static bool g_foreground = false;
static bool g_debug      = false;

void mf_log(int prio, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_foreground)
    {
        fprintf(stderr, "%s\n", buf);
    }
    else
    {
        syslog(prio, "%s", buf);
    }
}

#define LOG_I(...) mf_log(LOG_INFO,    __VA_ARGS__)
#define LOG_W(...) mf_log(LOG_WARNING, __VA_ARGS__)
#define LOG_E(...) mf_log(LOG_ERR,     __VA_ARGS__)

static protothread_t         g_pts;
static int                   g_listen_fd = -1;
static char                  g_chan_tick;
static volatile sig_atomic_t g_quit = 0;
static mf_http_t             g_http;
static mf_plugin_registry_t  g_plugins;
static mf_daemon_config_t    g_cfg;

/* A module that captures history must also say how long to keep it. */
static void warn_capture_without_policy(void)
{
    int i;

    for (i = 0; i < g_plugins.nops; i++)
    {
        const mf_plugin_ops_t *ops = g_plugins.ops[i];
        const char *d = ops->describe ? ops->describe() : NULL;

        if (d && strstr(d, "\"capture\"") &&
            mf_capture_spec(ops, NULL, 0, NULL, NULL, NULL) != 0)
            LOG_W("plugin %s/%s: capture has no retention_days pruning"
                  " policy; history disabled for it", ops->kind, ops->driver);
    }
}

/* Capture spec for a module the migration finds but no longer runs. */
static const char *history_spec_for(const char *kind, const char *driver)
{
    static char spec[2048];

    if (mf_capture_spec(mf_plugins_find(&g_plugins, kind, driver),
                        spec, sizeof(spec), NULL, NULL, NULL) != 0)
        return NULL;
    return spec;
}

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
    if (!colon)
    {
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
    if (parse_listen(spec, host, sizeof(host), &port) < 0 || port <= 0 || port > 65535)
    {
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
    if (rc != 0)
    {
        LOG_E("getaddrinfo(%s,%d): %s", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next)
    {
        fd = socket(p->ai_family,
                    p->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    p->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0)
        {
            if (listen(fd, 16) == 0) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0)
    {
        LOG_E("listen on %s:%d failed: %s", host, port, strerror(errno));
        return -1;
    }
    LOG_I("listening on %s:%d", host[0] ? host : "*", port);
    return fd;
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
        "          [--plugin-dir DIR] [--http-idle-s SEC] [--debug] [--help]\n"
        "\n"
        "  --listen      bind address (default %s)\n"
        "  --foreground  do not daemonize; log to stderr\n"
        "  --config      path to moonflared.json (load is PR-3; accepted now)\n"
        "  --plugin-dir  directory of libmf_*.so (optional)\n"
        "  --http-idle-s idle close seconds (default %.0f)\n"
        "  --debug       extra accept/idle logging\n"
        "\n"
        "Does not bind 5252 or 5253 (xd_bmsd / jkbmsd soak ports).\n"
        "Example:\n"
        "  %s --listen 127.0.0.1:5250 --foreground\n",
        prog, DEFAULT_LISTEN, MF_HTTP_IDLE_S, prog);
}

int main(int argc, char **argv)
{
    const char *listen_spec = DEFAULT_LISTEN;
    int listen_from_cli = 0;
    const char *config_path = NULL;
    const char *plugin_dir = NULL;
    double http_idle_s = MF_HTTP_IDLE_S;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--listen") && i + 1 < argc)
        {
            listen_spec = argv[++i];
            listen_from_cli = 1;
        }
        else if (!strcmp(argv[i], "--config") && i + 1 < argc)
        {
            config_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--plugin-dir") && i + 1 < argc)
        {
            plugin_dir = argv[++i];
        }
        else if (!strcmp(argv[i], "--http-idle-s") && i + 1 < argc)
        {
            http_idle_s = atof(argv[++i]);
            if (http_idle_s <= 0.0)
                http_idle_s = MF_HTTP_IDLE_S;
        }
        else if (!strncmp(argv[i], "--http-idle-s=", 14))
        {
            http_idle_s = atof(argv[i] + 14);
            if (http_idle_s <= 0.0)
                http_idle_s = MF_HTTP_IDLE_S;
        }
        else if (!strcmp(argv[i], "--foreground"))
        {
            g_foreground = true;
        }
        else if (!strcmp(argv[i], "--debug"))
        {
            g_debug = true;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!g_foreground)
        openlog("moonflared", LOG_PID, LOG_DAEMON);

    mf_config_defaults(&g_cfg);
    {
        const char *state = mf_config_state_path();
        int rc = mf_config_load_state(&g_cfg);

        if (rc > 0)
            LOG_I("config: %s", state);
        else
        {
            if (rc < 0)
            {
                /* Keep the bad file for inspection; seed a fresh one. */
                char bad[300];

                snprintf(bad, sizeof(bad), "%s.bad", state);
                LOG_W("config: %s unreadable; moved to %s", state, bad);
                (void)rename(state, bad);
                mf_config_defaults(&g_cfg);
            }
            if (mf_config_load(config_path, &g_cfg) != 0)
                LOG_W("config load failed; using defaults");
            if (mf_config_load_overlay(&g_cfg) != 0)
                LOG_W("settings overlay not loaded");
            if (mf_config_save_state(&g_cfg) == 0)
                LOG_I("config: seeded %s from %s", state,
                      config_path ? config_path : "the config search path");
            else
                LOG_W("config: cannot write %s; changes will not persist",
                      state);
        }
    }
    if (!listen_from_cli && g_cfg.listen[0])
        listen_spec = g_cfg.listen;
    if (!plugin_dir && g_cfg.plugin_dir[0])
        plugin_dir = g_cfg.plugin_dir;
    if (plugin_dir)
        (void)mf_plugins_load_dir(&g_plugins, plugin_dir);
    warn_capture_without_policy();

    g_listen_fd = listen_tcp(listen_spec);
    if (g_listen_fd < 0)
        return 1;
    snprintf(g_cfg.listen, sizeof(g_cfg.listen), "%s", listen_spec);

    if (!g_foreground)
    {
        if (daemon(0, 0) < 0)
        {
            LOG_E("daemon(): %s", strerror(errno));
            return 1;
        }
    }

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_quit;
        sigemptyset(&sa.sa_mask);
        /* no SA_RESTART: select() must return so g_quit is observed */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGCHLD, SIG_DFL); /* reaped with waitpid WNOHANG in the loop */
    }
    if (g_cfg.gatt_bin[0])
        setenv("MF_GATT_BIN", g_cfg.gatt_bin, 1);

    g_pts = protothread_create();
    if (!g_pts)
    {
        LOG_E("protothread_create failed");
        close(g_listen_fd);
        return 1;
    }

    mf_devices_init(g_pts, &g_chan_tick, &g_quit, &g_plugins);
    if (g_cfg.history.enabled && g_cfg.history.dir[0])
    {
        if (mf_history_open(g_cfg.history.dir) != 0)
            LOG_W("history: open %s failed", g_cfg.history.dir);
        else
            LOG_I("history: per-module files in %s", g_cfg.history.dir);
    }
    {
        char err[96];
        int rc = mf_devices_apply_config(g_cfg.devices, g_cfg.n_devices,
                                         err, sizeof(err));
        if (rc < 0)
            LOG_W("startup config apply: %s", err[0] ? err : "failed");
    }
    /* Modules are registered now: move a pre-0.5 shared history file into
     * their own files (once; an interrupted run resumes next start). */
    if (mf_history_is_open() && g_cfg.history.path[0] &&
        access(g_cfg.history.path, F_OK) == 0)
    {
        int rows;

        LOG_I("history: migrating %s", g_cfg.history.path);
        rows = mf_history_migrate(g_cfg.history.path, history_spec_for);
        if (rows < 0)
            LOG_W("history: migration incomplete; will resume next start");
        else
            LOG_I("history: migrated %d samples", rows);
    }
    mf_rest_set_live_config(&g_cfg, config_path);
    mf_rest_init();
    mf_http_init(&g_http, g_pts, &g_chan_tick, &g_quit, g_listen_fd,
                 http_idle_s, g_debug);
    mf_http_start(&g_http);
    while (protothread_run(g_pts))
        ; /* park accept PT on g_chan_tick before the first select */

    while (!g_quit)
    {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        int maxfd = -1;
        mf_http_prepare_fds(&g_http, &rset, &wset, &maxfd);
        mf_devices_prepare_fds(&rset, &wset, &maxfd);
        mf_discover_prepare_fds(&rset, &wset, &maxfd);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = (suseconds_t)TICK_US;

        int n = select(maxfd + 1, &rset, &wset, NULL, &tv);
        if (n < 0 && errno != EINTR)
        {
            LOG_E("select: %s", strerror(errno));
            break;
        }

        /* Tick-or-fd: always broadcast. Listen- or conn-ready also
         * wakes the same channel (KD 10 / 18). */
        (void)n;
        pt_broadcast(g_pts, &g_chan_tick);
        while (protothread_run(g_pts))
            ;
        mf_devices_apply_pending();
        mf_discover_step();
        if (mf_history_is_open())
        {
            mf_history_flush_slice(32);
            mf_history_prune_slice((double)time(NULL), 500);
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }

    LOG_I("shutting down");
    g_quit = 1;
    mf_devices_request_stop_all();
    pt_broadcast(g_pts, &g_chan_tick);
    while (protothread_run(g_pts))
        ;
    mf_history_close();
    mf_discover_close();
    mf_http_close_all(&g_http);
    mf_plugins_unload(&g_plugins);
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    protothread_free(g_pts);
    if (!g_foreground)
        closelog();
    return 0;
}
