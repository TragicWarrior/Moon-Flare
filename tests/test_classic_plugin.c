#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "mf_plugin.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static pid_t g_child = -1;
static int g_port;

static void stop_fake(void);

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void spawn_fake(const char *bin)
{
    int sp[2];
    FILE *fp;
    char line[128];
    unsigned port = 0;

    if (pipe(sp) != 0)
        exit(1);
    g_child = fork();
    if (g_child < 0)
        exit(1);
    if (g_child == 0)
    {
        dup2(sp[1], STDOUT_FILENO);
        close(sp[0]);
        close(sp[1]);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    close(sp[1]);
    atexit(stop_fake);
    fp = fdopen(sp[0], "r");
    if (!fp || !fgets(line, sizeof(line), fp))
    {
        fprintf(stderr, "FAIL: no listen line\n");
        exit(1);
    }
    if (sscanf(line, "fake_classic: listening on 127.0.0.1:%u", &port) != 1)
    {
        fprintf(stderr, "FAIL: parse %s", line);
        exit(1);
    }
    g_port = (int)port;
    fclose(fp);
}

static void stop_fake(void)
{
    int i;
    if (g_child <= 0)
        return;
    kill(g_child, SIGTERM);
    for (i = 0; i < 20; i++)
    {
        if (waitpid(g_child, NULL, WNOHANG) == g_child)
        {
            g_child = -1;
            return;
        }
        usleep(20000);
    }
    kill(g_child, SIGKILL);
    waitpid(g_child, NULL, 0);
    g_child = -1;
}

static int drive(const mf_plugin_ops_t *ops, void *ctx, int max_ms)
{
    int waited = 0;
    while (waited <= max_ms)
    {
        int fd = ops->fd(ctx);
        unsigned mask = ops->select_mask(ctx);
        struct pollfd p;
        int to = 50;
        mf_step_t st;
        if (fd >= 0 && mask)
        {
            p.fd = fd;
            p.events = 0;
            if (mask & MF_IO_WANT_READ)
                p.events = (short)(p.events | POLLIN);
            if (mask & MF_IO_WANT_WRITE)
                p.events = (short)(p.events | POLLOUT);
            (void)poll(&p, 1, to);
        }
        else
        {
            usleep((useconds_t)to * 1000);
        }
        waited += to;
        st = ops->step(ctx);
        if (st == MF_STEP_UPDATED)
            return 1;
    }
    return 0;
}

static double json_num(const char *js, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return -1.0;
    p = strchr(p, ':');
    if (!p)
        return -1.0;
    return atof(p + 1);
}

int main(int argc, char **argv)
{
    const char *fake_bin, *so_path;
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    const mf_plugin_ops_t *ops = NULL;
    size_t n;
    void *ctx;
    char spec[256];
    char json[1024];
    char err[96];
    int fd1, fd2;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s fake_classic libmf_charger_classic.so\n", argv[0]);
        return 2;
    }
    fake_bin = argv[1];
    so_path = argv[2];
    spawn_fake(fake_bin);

    dl = dlopen(so_path, RTLD_NOW);
    if (!dl)
    {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", so_path, dlerror());
        stop_fake();
        return 1;
    }
    entries = (size_t (*)(const mf_plugin_ops_t **))dlsym(dl, "mf_plugin_entries");
    if (!entries)
    {
        fprintf(stderr, "FAIL: missing mf_plugin_entries\n");
        stop_fake();
        return 1;
    }
    n = entries(&ops);
    CHECK(n == 1, "n=1");
    CHECK(ops && strcmp(ops->kind, "charger") == 0, "kind charger");
    CHECK(ops && strcmp(ops->driver, "classic") == 0, "driver classic");
    CHECK(ops && ops->abi == MF_PLUGIN_ABI, "abi");

    snprintf(spec, sizeof(spec),
             "{\"name\":\"classic-1\",\"kind\":\"charger\",\"driver\":\"classic\","
             "\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":%d,\"unit_id\":10},"
             "\"poll_interval_s\":1}",
             g_port);
    err[0] = '\0';
    ctx = ops->open(spec, err, sizeof(err));
    CHECK(ctx != NULL, "open unit 10");
    CHECK(drive(ops, ctx, 2000), "first FC3");
    CHECK(ops->get_reading(ctx, json, sizeof(json)) == 0, "get_reading");
    CHECK(ops->caps && ops->caps(ctx) ==
          (MF_CAP_READ | MF_CAP_PROBE | MF_CAP_AUTO_NET), "caps read+probe+auto_net");
    CHECK(json_num(json, "charging_watts") == 2500.0, "watts 2500");
    CHECK(json_num(json, "battery_voltage_v") > 127.4 &&
          json_num(json, "battery_voltage_v") < 127.6, "volts 127.5");
    CHECK(json_num(json, "kwh_today") > 3.39 && json_num(json, "kwh_today") < 3.41,
          "kwh 3.4");
    CHECK(strstr(json, "\"charge_stage\":\"Absorb\"") != NULL, "Absorb");
    CHECK(strstr(json, "\"classic_mac\":\"55:66:33:44:11:22\"") != NULL,
          "UNIT_MAC");
    CHECK(json_num(json, "unit_id") == 10.0, "unit_id 10");
    fd1 = ops->fd(ctx);

    /* keep-alive: same fd after next poll */
    CHECK(drive(ops, ctx, 2000), "second FC3");
    fd2 = ops->fd(ctx);
    CHECK(fd1 >= 0 && fd1 == fd2, "keep-alive same fd");
    ops->close(ctx);

    /* unit 1 */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":%d,\"unit_id\":1},"
             "\"poll_interval_s\":1}",
             g_port);
    ctx = ops->open(spec, err, sizeof(err));
    CHECK(ctx != NULL, "open unit 1");
    CHECK(drive(ops, ctx, 2000), "FC3 unit 1");
    CHECK(ops->get_reading(ctx, json, sizeof(json)) == 0, "reading unit 1");
    CHECK(json_num(json, "charging_watts") == 2500.0, "watts unit 1");
    CHECK(json_num(json, "unit_id") == 1.0, "unit_id 1");
    ops->close(ctx);

    /* configured 99 is ignored by fake → fallback to 10 */
    snprintf(spec, sizeof(spec),
             "{\"modbus\":{\"ip\":\"127.0.0.1\",\"port\":%d,\"unit_id\":99},"
             "\"poll_interval_s\":1}",
             g_port);
    ctx = ops->open(spec, err, sizeof(err));
    CHECK(ctx != NULL, "open unit 99");
    CHECK(drive(ops, ctx, 4000), "FC3 fallback 99→10");
    CHECK(ops->get_reading(ctx, json, sizeof(json)) == 0, "reading fallback");
    CHECK(json_num(json, "charging_watts") == 2500.0, "watts fallback");
    CHECK(json_num(json, "unit_id") == 10.0, "fell back to unit 10");
    CHECK(ops->action(ctx, "anything", "{}", err, sizeof(err)) == MF_ERR_UNSUPPORTED,
          "read-only actions");
    CHECK(ops->put_settings(ctx, "{}", err, sizeof(err)) == MF_ERR_UNSUPPORTED,
          "read-only settings");
    ops->close(ctx);

    dlclose(dl);
    stop_fake();
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("classic_plugin: ok\n");
    return 0;
}
