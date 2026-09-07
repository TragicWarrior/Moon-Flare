#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "mf_plugin.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static pid_t g_kids[8];
static int g_nkids;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void stop_kids(void)
{
    int i;
    for (i = 0; i < g_nkids; i++) {
        if (g_kids[i] <= 0)
            continue;
        kill(g_kids[i], SIGTERM);
        waitpid(g_kids[i], NULL, 0);
        g_kids[i] = -1;
    }
    g_nkids = 0;
}

static int spawn_fake(const char *bin, const char *name_reg)
{
    int sp[2];
    FILE *fp;
    char line[128];
    unsigned port = 0;
    pid_t child;

    if (g_nkids >= (int)(sizeof(g_kids) / sizeof(g_kids[0])))
        exit(1);
    if (pipe(sp) != 0)
        exit(1);
    child = fork();
    if (child < 0)
        exit(1);
    if (child == 0) {
        dup2(sp[1], STDOUT_FILENO);
        close(sp[0]);
        close(sp[1]);
        if (name_reg && name_reg[0])
            execl(bin, bin, name_reg, (char *)NULL);
        else
            execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    close(sp[1]);
    g_kids[g_nkids++] = child;
    fp = fdopen(sp[0], "r");
    if (!fp || !fgets(line, sizeof(line), fp)) {
        fprintf(stderr, "FAIL: no listen line\n");
        exit(1);
    }
    if (sscanf(line, "fake_classic: listening on 127.0.0.1:%u", &port) != 1) {
        fprintf(stderr, "FAIL: parse %s", line);
        exit(1);
    }
    fclose(fp);
    return (int)port;
}

static int listen_loopback(int *port_out)
{
    int fd, one = 1;
    struct sockaddr_in a;
    socklen_t alen = (socklen_t)sizeof(a);
    int flags;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    *port_out = (int)ntohs(a.sin_port);
    return fd;
}

static int decoy_accepted(int fd)
{
    struct sockaddr_in a;
    socklen_t n = (socklen_t)sizeof(a);
    int c = accept(fd, (struct sockaddr *)&a, &n);
    if (c >= 0) {
        close(c);
        return 1;
    }
    return 0;
}

static int count_probe_fds(const mf_plugin_ops_t *ops, void *job)
{
    fd_set r, w;
    int maxfd = -1, n = 0, i;
    FD_ZERO(&r);
    FD_ZERO(&w);
    ops->probe_prepare_fds(job, &r, &w, &maxfd);
    if (maxfd < 0)
        return 0;
    for (i = 0; i <= maxfd; i++) {
        if (FD_ISSET(i, &r) || FD_ISSET(i, &w))
            n++;
    }
    return n;
}

static int drive_probe(const mf_plugin_ops_t *ops, void *job, int max_ms,
                       int *max_if)
{
    int waited = 0;
    if (max_if)
        *max_if = count_probe_fds(ops, job);
    while (waited <= max_ms) {
        fd_set r, w;
        int maxfd = -1, nfd, to = 50;
        mf_step_t st;
        struct timeval tv;

        FD_ZERO(&r);
        FD_ZERO(&w);
        ops->probe_prepare_fds(job, &r, &w, &maxfd);
        nfd = 0;
        if (maxfd >= 0) {
            int i;
            for (i = 0; i <= maxfd; i++) {
                if (FD_ISSET(i, &r) || FD_ISSET(i, &w))
                    nfd++;
            }
        }
        if (max_if && nfd > *max_if)
            *max_if = nfd;
        if (maxfd >= 0) {
            tv.tv_sec = 0;
            tv.tv_usec = (suseconds_t)to * 1000;
            (void)select(maxfd + 1, &r, &w, NULL, &tv);
        } else {
            usleep((useconds_t)to * 1000);
        }
        waited += to;
        st = ops->probe_step(job);
        nfd = count_probe_fds(ops, job);
        if (max_if && nfd > *max_if)
            *max_if = nfd;
        if (st == MF_STEP_UPDATED)
            return 1;
        if (st == MF_STEP_ERROR)
            return 0;
    }
    return 0;
}

static const mf_plugin_ops_t *load_ops(const char *so_path, void **dl_out)
{
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    const mf_plugin_ops_t *ops = NULL;
    size_t n;

    dl = dlopen(so_path, RTLD_NOW);
    if (!dl) {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", so_path, dlerror());
        return NULL;
    }
    entries = (size_t (*)(const mf_plugin_ops_t **))dlsym(dl, "mf_plugin_entries");
    if (!entries) {
        fprintf(stderr, "FAIL: missing mf_plugin_entries\n");
        dlclose(dl);
        return NULL;
    }
    n = entries(&ops);
    if (n != 1 || !ops) {
        fprintf(stderr, "FAIL: entries n=%zu\n", n);
        dlclose(dl);
        return NULL;
    }
    *dl_out = dl;
    return ops;
}

static int results_empty(const char *js)
{
    return strstr(js, "\"results\":[]") != NULL;
}

int main(int argc, char **argv)
{
    const char *fake_bin, *so_path;
    void *dl = NULL;
    const mf_plugin_ops_t *ops;
    void *job;
    char spec[1024];
    char json[2048];
    char err[96];
    int fake_port, bad_port, decoy_port;
    int decoy_fd;
    int max_if;
    int stall[9];
    int stall_fd[9];
    int i;
    char cand[512];
    size_t off;

    if (argc < 3) {
        fprintf(stderr, "usage: %s fake_classic libmf_charger_classic.so\n",
                argv[0]);
        return 2;
    }
    fake_bin = argv[1];
    so_path = argv[2];
    atexit(stop_kids);

    ops = load_ops(so_path, &dl);
    if (!ops)
        return 1;
    CHECK(ops->probe_start && ops->probe_step && ops->probe_prepare_fds &&
          ops->probe_result && ops->probe_close, "probe ABI");

    fake_port = spawn_fake(fake_bin, NULL);

    /* 1. reachable last-known does not scan a decoy candidate */
    decoy_fd = listen_loopback(&decoy_port);
    CHECK(decoy_fd >= 0, "decoy listen");
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":%d,\"unit_id\":10,"
             "\"mac\":\"55:66:33:44:11:22\"},\"auto_net\":true,"
             "\"candidates\":[\"127.0.0.1:%d\"]}",
             fake_port, decoy_port);
    err[0] = '\0';
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start last-known");
    CHECK(count_probe_fds(ops, job) <= 1, "last-known uses one fd");
    CHECK(drive_probe(ops, job, 2000, &max_if), "last-known probe");
    CHECK(max_if <= 1, "last-known never scanned");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result last-known");
    CHECK(strstr(json, "\"status\":\"done\"") != NULL, "done last-known");
    CHECK(strstr(json, "\"mac\":\"55:66:33:44:11:22\"") != NULL, "mac last-known");
    CHECK(strstr(json, "127.0.0.1:") != NULL, "endpoint last-known");
    CHECK(!decoy_accepted(decoy_fd), "decoy not contacted");
    ops->probe_close(job);

    /* 2. miss + auto_net + no MAC finds the fake */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":1,\"unit_id\":10},"
             "\"auto_net\":true,\"candidates\":[\"127.0.0.1:%d\",\"127.0.0.1:%d\"]}",
             decoy_port, fake_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start miss/no-mac");
    CHECK(drive_probe(ops, job, 4000, &max_if), "scan finds fake");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result no-mac");
    CHECK(strstr(json, "\"mac\":\"55:66:33:44:11:22\"") != NULL, "learned mac");
    CHECK(strstr(json, "\"unit_id\":10") != NULL ||
          strstr(json, "\"unit_id\":1") != NULL, "unit id");
    ops->probe_close(job);

    /* 3. miss + matching MAC finds fake */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":1,"
             "\"mac\":\"55:66:33:44:11:22\"},"
             "\"auto_net\":true,\"candidates\":[\"127.0.0.1:%d\"]}",
             fake_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start match-mac");
    CHECK(drive_probe(ops, job, 4000, NULL), "scan match-mac");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result match-mac");
    CHECK(strstr(json, "\"mac\":\"55:66:33:44:11:22\"") != NULL, "matched mac");
    ops->probe_close(job);

    /* 4. miss + wrong MAC does not report the fake */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":1,"
             "\"mac\":\"00:11:22:33:44:55\"},"
             "\"auto_net\":true,\"candidates\":[\"127.0.0.1:%d\"]}",
             fake_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start wrong-mac");
    CHECK(drive_probe(ops, job, 4000, NULL), "scan wrong-mac finishes");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result wrong-mac");
    CHECK(results_empty(json), "wrong mac rejected");
    ops->probe_close(job);

    /* 5. 4209 mismatch rejected even with no stored MAC */
    bad_port = spawn_fake(fake_bin, "0x0000");
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":%d},"
             "\"auto_net\":true,\"candidates\":[]}",
             bad_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start 4209");
    CHECK(drive_probe(ops, job, 4000, NULL), "4209 probe finishes");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result 4209");
    CHECK(results_empty(json), "4209 mismatch rejected");
    ops->probe_close(job);

    /* 5b. scan skips the bad type-check and still finds the real fake */
    snprintf(spec, sizeof(spec),
             "{\"auto_net\":true,\"candidates\":[\"127.0.0.1:%d\",\"127.0.0.1:%d\"]}",
             bad_port, fake_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start mixed");
    CHECK(drive_probe(ops, job, 4000, NULL), "mixed scan");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result mixed");
    CHECK(strstr(json, "\"mac\":\"55:66:33:44:11:22\"") != NULL,
          "mixed found real Classic");
    ops->probe_close(job);

    /* 6. ≥9 candidates never exceed 8 in-flight */
    off = 0;
    cand[0] = '\0';
    for (i = 0; i < 9; i++) {
        stall_fd[i] = listen_loopback(&stall[i]);
        CHECK(stall_fd[i] >= 0, "stall listen");
        off += (size_t)snprintf(cand + off, sizeof(cand) - off, "%s\"127.0.0.1:%d\"",
                                i ? "," : "", stall[i]);
    }
    snprintf(spec, sizeof(spec),
             "{\"auto_net\":true,\"candidates\":[%s]}", cand);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start 9-wide");
    max_if = count_probe_fds(ops, job);
    CHECK(max_if <= 8, "start ≤8");
    (void)drive_probe(ops, job, 400, &max_if);
    CHECK(max_if <= 8, "never exceed 8 in-flight");
    CHECK(max_if == 8, "window filled to 8");
    ops->probe_close(job);
    for (i = 0; i < 9; i++)
        close(stall_fd[i]);

    /* 7. auto_net false does not scan after last-known miss */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":1},"
             "\"auto_net\":false,\"candidates\":[\"127.0.0.1:%d\"]}",
             fake_port);
    CHECK(ops->probe_start(spec, &job, err, sizeof(err)) == MF_OK, "start no-auto");
    CHECK(drive_probe(ops, job, 2000, NULL), "no-auto finishes");
    CHECK(ops->probe_result(job, json, sizeof(json)) == 0, "result no-auto");
    CHECK(results_empty(json), "auto_net false does not scan");
    ops->probe_close(job);

    close(decoy_fd);
    dlclose(dl);
    stop_kids();
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("classic_discover: ok\n");
    return 0;
}
