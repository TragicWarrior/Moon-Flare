/*
 * PR-5 merge bar against a live moonflared + libmf_demo.so:
 * status empty arrays, drivers from dlopen, POST open, DELETE 202 then 404.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); g_fail++; } while (0)

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
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    if (getsockname(s, (struct sockaddr *)&a, &sl) != 0) {
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
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, (int)(timeout_s * 1000.0)) <= 0) {
        close(fd);
        return -1;
    }
    {
        int err = 0;
        socklen_t el = (socklen_t)sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

static int send_all(int fd, const char *s)
{
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = write(fd, s + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int recv_http(int fd, char *buf, size_t cap, size_t *out, double timeout_s)
{
    struct pollfd pfd;
    *out = 0;
    buf[0] = '\0';
    for (;;) {
        ssize_t n;
        char *hdr;
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)(timeout_s * 1000.0)) <= 0)
            return -1;
        n = read(fd, buf + *out, cap - 1 - *out);
        if (n <= 0)
            break;
        *out += (size_t)n;
        buf[*out] = '\0';
        hdr = strstr(buf, "\r\n\r\n");
        if (hdr) {
            unsigned long cl = 0;
            const char *p = strstr(buf, "Content-Length:");
            if (p)
                cl = strtoul(p + 15, NULL, 10);
            if (*out >= (size_t)(hdr - buf) + 4 + cl)
                return 0;
        }
        timeout_s = 1.0;
    }
    return 0;
}

static int http_exchange(int port, const char *req, char *buf, size_t cap)
{
    size_t n = 0;
    int fd = tcp_connect(port, 1.0);
    if (fd < 0)
        return -1;
    if (send_all(fd, req) < 0) {
        close(fd);
        return -1;
    }
    if (recv_http(fd, buf, cap, &n, 1.5) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int status_of(const char *resp)
{
    int st = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &st) != 1)
        return -1;
    return st;
}

static const char *body_of(const char *resp)
{
    const char *b = strstr(resp, "\r\n\r\n");
    return b ? b + 4 : resp;
}

static pid_t spawn_daemon(const char *bin, const char *plugindir, int port,
                          int logfd, const char *cfgpath)
{
    pid_t pid = fork();
    char spec[64];
    char portstr[16];
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (logfd >= 0) {
            dup2(logfd, STDERR_FILENO);
            if (logfd != STDERR_FILENO)
                close(logfd);
        }
        snprintf(spec, sizeof(spec), "127.0.0.1:%d", port);
        snprintf(portstr, sizeof(portstr), "%d", port);
        if (cfgpath && cfgpath[0])
            execl(bin, bin, "--listen", spec, "--foreground",
                  "--plugin-dir", plugindir, "--config", cfgpath,
                  (char *)NULL);
        else
            execl(bin, bin, "--listen", spec, "--foreground",
                  "--plugin-dir", plugindir, (char *)NULL);
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
    for (i = 0; i < 40; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        sleep_s(0.05);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static uint64_t gen_of_body(const char *body)
{
    const char *p = strstr(body, "\"config_gen\":");
    if (!p)
        return 0;
    return strtoull(p + 13, NULL, 10);
}

static int write_startup_cfg(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs("{\n"
          "  \"devices\": [{\n"
          "    \"uuid\": \"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeee01\",\n"
          "    \"name\": \"pack-cfg\",\n"
          "    \"kind\": \"battery\",\n"
          "    \"driver\": \"demo\",\n"
          "    \"enabled\": true,\n"
          "    \"poll_interval_s\": 2.0,\n"
          "    \"bus\": \"demo\"\n"
          "  }]\n"
          "}\n", f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *bin, *plugindir;
    int port, logfd, i, ready = 0, fd;
    pid_t pid;
    char resp[8192];
    char uuid[40];
    char req[2048];
    char delpath[256];
    const char *cfgpath = "/tmp/mf-http-devices-cfg.json";

    if (argc < 3) {
        fprintf(stderr, "usage: %s moonflared plugin-dir\n", argv[0]);
        return 2;
    }
    bin = argv[1];
    plugindir = argv[2];
    port = pick_port();
    if (port <= 0) {
        FAIL("pick_port");
        return 1;
    }
    if (write_startup_cfg(cfgpath) != 0) {
        FAIL("write startup cfg");
        return 1;
    }
    logfd = open("/tmp/mf-http-devices.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid = spawn_daemon(bin, plugindir, port, logfd, cfgpath);
    if (logfd >= 0)
        close(logfd);
    if (pid < 0) {
        FAIL("fork");
        return 1;
    }
    for (i = 0; i < 50; i++) {
        fd = tcp_connect(port, 0.1);
        if (fd >= 0) {
            close(fd);
            ready = 1;
            break;
        }
        sleep_s(0.05);
    }
    if (!ready) {
        FAIL("daemon listen");
        stop_daemon(pid);
        return 1;
    }

    if (http_exchange(port,
                      "GET /api/v1/status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                      resp, sizeof(resp)) < 0)
        FAIL("GET status");
    else if (status_of(resp) != 200)
        FAIL("status not 200");
    else if (!strstr(body_of(resp), "\"inverters\":[]") ||
             !strstr(body_of(resp), "\"phantoms\":[]"))
        FAIL("status missing empty inverters/phantoms");
    else if (!strstr(body_of(resp), "pack-cfg"))
        FAIL("startup did not open pack-cfg");

    if (http_exchange(port,
                      "GET /api/v1/drivers HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                      resp, sizeof(resp)) < 0)
        FAIL("GET drivers");
    else if (status_of(resp) != 200)
        FAIL("drivers not 200");
    else if (!strstr(body_of(resp), "\"kind\":\"battery\"") ||
             !strstr(body_of(resp), "\"driver\":\"demo\"") ||
             !strstr(body_of(resp), "\"kind\":\"charger\""))
        FAIL("drivers missing demo battery+charger");

    {
        const char *body =
            "{\"name\":\"pack-demo\",\"kind\":\"battery\",\"driver\":\"demo\"}";
        snprintf(req, sizeof(req),
                 "POST /api/v1/devices HTTP/1.1\r\nHost: x\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n%s",
                 strlen(body), body);
    }
    if (http_exchange(port, req, resp, sizeof(resp)) < 0)
        FAIL("POST device");
    else if (status_of(resp) != 201)
        FAIL("POST not 201");
    else {
        const char *loc = strstr(resp, "Location:");
        const char *slash;
        uuid[0] = '\0';
        if (!loc)
            FAIL("POST missing Location");
        else {
            slash = strrchr(loc, '/');
            if (!slash)
                FAIL("Location path");
            else {
                int n = 0;
                slash++;
                while (slash[n] && slash[n] != '\r' && n < 36)
                    n++;
                if (n != 36)
                    FAIL("uuid not 36");
                memcpy(uuid, slash, 36);
                uuid[36] = '\0';
            }
        }
    }

    if (uuid[0] && http_exchange(port,
            "GET /api/v1/status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            resp, sizeof(resp)) == 0) {
        if (!strstr(body_of(resp), "pack-demo"))
            FAIL("status missing posted device");
        if (!strstr(body_of(resp), "pack_voltage_v"))
            FAIL("status missing demo reading");
    }

    if (uuid[0]) {
        snprintf(req, sizeof(req),
                 "GET /api/v1/devices/%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                 uuid);
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 || status_of(resp) != 200)
            FAIL("GET device");
        else if (!strstr(body_of(resp), "\"data\""))
            FAIL("device missing data");
    }

    {
        const char *body =
            "{\"name\":\"nope\",\"kind\":\"battery\",\"driver\":\"nope\"}";
        snprintf(req, sizeof(req),
                 "POST /api/v1/devices HTTP/1.1\r\nHost: x\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n%s",
                 strlen(body), body);
    }
    if (http_exchange(port, req, resp, sizeof(resp)) == 0 && status_of(resp) != 400)
        FAIL("unknown driver should 400");

    if (uuid[0]) {
        char req2[1024];
        {
            const char *sb = "{\"poll_interval_s\":0.1}";
            snprintf(req2, sizeof(req2),
                     "PUT /api/v1/devices/%s/settings HTTP/1.1\r\nHost: x\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     uuid, strlen(sb), sb);
        }
        if (http_exchange(port, req2, resp, sizeof(resp)) < 0 || status_of(resp) != 400)
            FAIL("PUT poll clamp");
        {
            const char *sb = "{\"poll_interval_s\":2.0}";
            snprintf(req2, sizeof(req2),
                     "PUT /api/v1/devices/%s/settings HTTP/1.1\r\nHost: x\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     uuid, strlen(sb), sb);
        }
        if (http_exchange(port, req2, resp, sizeof(resp)) < 0 || status_of(resp) != 200)
            FAIL("PUT poll 2.0");
        else if (!strstr(body_of(resp), "poll_interval_s"))
            FAIL("settings missing poll_interval_s");

        {
            const char *ab = "{\"key\":\"charge\",\"value\":false}";
            snprintf(req2, sizeof(req2),
                     "POST /api/v1/devices/%s/actions/set_switch HTTP/1.1\r\n"
                     "Host: x\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     uuid, strlen(ab), ab);
        }
        if (http_exchange(port, req2, resp, sizeof(resp)) < 0 || status_of(resp) != 200)
            FAIL("POST set_switch");
    }

    if (http_exchange(port,
                      "GET /api/v1/config HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                      resp, sizeof(resp)) < 0 || status_of(resp) != 200)
        FAIL("GET config");
    else if (!strstr(body_of(resp), "config_gen"))
        FAIL("config missing config_gen");
    else if (!strstr(body_of(resp), "pack-cfg"))
        FAIL("config missing startup pack-cfg");
    else if (uuid[0] && !strstr(body_of(resp), "pack-demo"))
        FAIL("config missing POSTed pack-demo");
    {
        uint64_t gen = gen_of_body(body_of(resp));
        char body[1024];
        snprintf(body, sizeof(body),
                 "{\"config_gen\":%llu,\"devices\":["
                 "{\"uuid\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeee01\","
                 "\"name\":\"pack-cfg\",\"kind\":\"battery\",\"driver\":\"demo\","
                 "\"enabled\":true,\"poll_interval_s\":2.0,\"bus\":\"demo\"},"
                 "{\"uuid\":\"%s\",\"name\":\"pack-demo\",\"kind\":\"battery\","
                 "\"driver\":\"demo\",\"enabled\":true,\"poll_interval_s\":2.0},"
                 "{\"uuid\":\"cccccccc-dddd-4eee-8fff-aaaaaaaaaa03\","
                 "\"name\":\"charger-cfg\",\"kind\":\"charger\",\"driver\":\"demo\","
                 "\"enabled\":true,\"poll_interval_s\":2.0}]}",
                 (unsigned long long)gen, uuid[0] ? uuid : "00000000-0000-4000-8000-000000000000");
        snprintf(req, sizeof(req),
                 "PUT /api/v1/config HTTP/1.1\r\nHost: x\r\n"
                 "If-Match: \"%llu\"\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 (unsigned long long)gen, strlen(body), body);
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 ||
            (status_of(resp) != 200 && status_of(resp) != 202))
            FAIL("PUT config ADD charger-cfg");
        else if (http_exchange(port,
                 "GET /api/v1/status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                 resp, sizeof(resp)) == 0 &&
                 !strstr(body_of(resp), "charger-cfg"))
            FAIL("status missing charger-cfg after PUT");
    }
    if (http_exchange(port,
                      "GET /api/v1/config HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                      resp, sizeof(resp)) == 0 && status_of(resp) == 200) {
        uint64_t gen = gen_of_body(body_of(resp));
        const char *body = "{\"listen\":\"127.0.0.1:99999\"}";
        snprintf(req, sizeof(req),
                 "PUT /api/v1/config HTTP/1.1\r\nHost: x\r\n"
                 "If-Match: \"%llu\"\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 (unsigned long long)gen, strlen(body), body);
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 || status_of(resp) != 500)
            FAIL("PUT listen invalid port should 500");
        if (http_exchange(port,
                "GET /api/v1/status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                resp, sizeof(resp)) == 0 && !strstr(body_of(resp), "pack-cfg"))
            FAIL("listen fail applied device diffs");
    }
    {
        const char *body = "{\"listen\":\"127.0.0.1:5250\"}";
        snprintf(req, sizeof(req),
                 "PUT /api/v1/config HTTP/1.1\r\nHost: x\r\n"
                 "If-Match: \"999\"\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 strlen(body), body);
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 || status_of(resp) != 409)
            FAIL("PUT config stale If-Match");
    }

    {
        const char *body = "{\"kind\":\"charger\",\"bus\":\"modbus\"}";
        snprintf(req, sizeof(req),
                 "POST /api/v1/discover HTTP/1.1\r\nHost: x\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n%s",
                 strlen(body), body);
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 || status_of(resp) != 202)
            FAIL("POST discover");
        if (http_exchange(port, req, resp, sizeof(resp)) < 0 || status_of(resp) != 409)
            FAIL("second POST discover");
        if (http_exchange(port,
                          "GET /api/v1/discover HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                          resp, sizeof(resp)) < 0 || status_of(resp) != 200)
            FAIL("GET discover");
        else if (!strstr(body_of(resp), "\"status\""))
            FAIL("discover missing status");
    }

    if (http_exchange(port,
                      "GET /api/v1/drivers HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                      resp, sizeof(resp)) < 0 || status_of(resp) != 200)
        FAIL("GET drivers after PR-10");
    else if (!strstr(body_of(resp), "\"driver\":\"demo\""))
        FAIL("drivers still lists demo");

    if (uuid[0]) {
        snprintf(delpath, sizeof(delpath),
                 "DELETE /api/v1/devices/%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                 uuid);
        if (http_exchange(port, delpath, resp, sizeof(resp)) < 0)
            FAIL("DELETE");
        else if (status_of(resp) != 202)
            FAIL("DELETE live not 202");

        /* dying: GET live list hides it */
        snprintf(req, sizeof(req),
                 "GET /api/v1/devices/%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                 uuid);
        if (http_exchange(port, req, resp, sizeof(resp)) == 0 && status_of(resp) != 404)
            FAIL("GET dying should 404");

        sleep_s(0.25);
        if (http_exchange(port, delpath, resp, sizeof(resp)) < 0)
            FAIL("DELETE after stop");
        else if (status_of(resp) != 404)
            FAIL("DELETE after free not 404");
    }

    stop_daemon(pid);
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed (see /tmp/mf-http-devices.log)\n", g_fail);
        return 1;
    }
    printf("http_devices: ok\n");
    return 0;
}
