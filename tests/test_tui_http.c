#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
static pid_t g_child = -1;
static int g_port;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stop_child(void)
{
    if (g_child <= 0)
        return;
    kill(g_child, SIGTERM);
    waitpid(g_child, NULL, 0);
    g_child = -1;
}

static void spawn_slow_http(int delay_ms)
{
    int sp[2];
    FILE *fp;
    char line[128];
    unsigned port = 0;

    if (pipe(sp) != 0)
        exit(1);
    g_child = fork();
    if (g_child == 0)
    {
        int srv, one = 1;
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        dup2(sp[1], STDOUT_FILENO);
        close(sp[0]);
        close(sp[1]);
        srv = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(srv, (struct sockaddr *)&a, sizeof(a));
        listen(srv, 8);
        getsockname(srv, (struct sockaddr *)&a, &al);
        printf("tui_http: listening on 127.0.0.1:%u\n",
               (unsigned)ntohs(a.sin_port));
        fflush(stdout);
        for (;;)
        {
            int cfd = accept(srv, NULL, NULL);
            char buf[1024];
            ssize_t n;
            if (cfd < 0)
                break;
            n = recv(cfd, buf, sizeof(buf) - 1, 0);
            (void)n;
            if (delay_ms > 0)
                usleep((useconds_t)delay_ms * 1000);
            {
                const char *body = "{\"seq\":1,\"ts\":1,\"batteries\":[],"
                                   "\"chargers\":[],\"inverters\":[],\"phantoms\":[]}";
                char resp[512];
                int m = snprintf(resp, sizeof(resp),
                                 "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                                 "Connection: keep-alive\r\n\r\n%s",
                                 strlen(body), body);
                send(cfd, resp, (size_t)m, MSG_NOSIGNAL);
            }
            /* keep-alive: leave socket; next accept is a new client */
            close(cfd);
        }
        _exit(0);
    }
    close(sp[1]);
    fp = fdopen(sp[0], "r");
    if (!fp || !fgets(line, sizeof(line), fp))
        exit(1);
    sscanf(line, "tui_http: listening on 127.0.0.1:%u", &port);
    g_port = (int)port;
    fclose(fp);
    atexit(stop_child);
}

static void drive(mf_http_cli_t *c, int ms)
{
    double t0 = now();
    while ((now() - t0) * 1000.0 < (double)ms)
    {
        fd_set r, w;
        int maxfd = -1;
        struct timeval tv = { 0, 20000 };
        FD_ZERO(&r);
        FD_ZERO(&w);
        mf_http_cli_prepare_fds(c, &r, &w, &maxfd);
        (void)select(maxfd + 1, &r, &w, NULL, &tv);
        mf_http_cli_pump(c, c->fd >= 0 && FD_ISSET(c->fd, &r),
                         c->fd >= 0 && FD_ISSET(c->fd, &w), now());
        if (c->have_body)
            break;
    }
}

int main(void)
{
    mf_http_cli_t cli;
    char body[1024];
    int rc;

    spawn_slow_http(150);
    mf_http_cli_init(&cli, "127.0.0.1", g_port);
    mf_http_cli_start(&cli, now());
    drive(&cli, 1000);
    CHECK(cli.state == MF_CONN_UP, "connected");

    rc = mf_http_cli_get(&cli, "/api/v1/status");
    CHECK(rc == 1, "first GET queued");
    rc = mf_http_cli_get(&cli, "/api/v1/status");
    CHECK(rc == 0, "second GET skipped while in flight");
    CHECK(cli.skipped >= 1, "skipped count");
    drive(&cli, 1000);
    CHECK(mf_http_cli_take_body(&cli, body, sizeof(body)) == 1, "got body");
    CHECK(strstr(body, "inverters") != NULL, "status json");

    stop_child();
    /* next pump should see close / fail and enter WAIT or CONNECTING */
    {
        fd_set r, w;
        int maxfd = -1;
        FD_ZERO(&r);
        FD_ZERO(&w);
        mf_http_cli_prepare_fds(&cli, &r, &w, &maxfd);
        mf_http_cli_pump(&cli, 1, 0, now());
    }
    CHECK(cli.state == MF_CONN_WAIT || cli.state == MF_CONN_CONNECTING,
          "reconnect FSM after peer gone");

    mf_http_cli_close(&cli);
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("tui_http: ok\n");
    return 0;
}
