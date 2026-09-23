#define _POSIX_C_SOURCE 200809L

#include "mf_plugin.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;
static pid_t g_child = -1;

#define MAC_A "28:D4:1E:A7:23:39"
#define MAC_B "AA:BB:CC:DD:EE:FF"

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void stop_fake(void);

static void spawn_fake(const char *bin)
{
    int sp[2];
    FILE *fp;
    char line[128];

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
        execl(bin, bin, "--adapter", "hci0", (char *)NULL);
        _exit(127);
    }
    close(sp[1]);
    atexit(stop_fake);
    fp = fdopen(sp[0], "r");
    if (!fp || !fgets(line, sizeof(line), fp))
    {
        fprintf(stderr, "FAIL: no fake_gatt listen line\n");
        exit(1);
    }
    if (!strstr(line, "@mf-gatt/hci0"))
    {
        fprintf(stderr, "FAIL: parse %s", line);
        exit(1);
    }
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
        int to = 20;
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

static void drive_both(const mf_plugin_ops_t *ops, void *a, void *b, int ms)
{
    int waited = 0;
    while (waited <= ms)
    {
        struct pollfd p[2];
        int n = 0, i, to = 20;
        void *ctx[2];
        ctx[0] = a;
        ctx[1] = b;
        for (i = 0; i < 2; i++)
        {
            int fd = ops->fd(ctx[i]);
            unsigned mask = ops->select_mask(ctx[i]);
            if (fd < 0 || !mask)
                continue;
            p[n].fd = fd;
            p[n].events = 0;
            if (mask & MF_IO_WANT_READ)
                p[n].events = (short)(p[n].events | POLLIN);
            if (mask & MF_IO_WANT_WRITE)
                p[n].events = (short)(p[n].events | POLLOUT);
            n++;
        }
        if (n)
            (void)poll(p, (nfds_t)n, to);
        else
            usleep((useconds_t)to * 1000);
        (void)ops->step(a);
        (void)ops->step(b);
        waited += to;
    }
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

static int json_true(const char *js, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return 0;
    p = strchr(p, ':');
    if (!p)
        return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return strncmp(p, "true", 4) == 0;
}

static int send_drop(const char *mac)
{
    int fd;
    struct sockaddr_un a;
    socklen_t alen;
    char req[128];
    ssize_t n, wr;
    int i;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    i = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "mf-gatt/hci0");
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)i);
    if (connect(fd, (struct sockaddr *)&a, alen) != 0)
    {
        close(fd);
        return 0;
    }
    n = snprintf(req, sizeof(req),
                 "{\"cmd\":\"drop\",\"address\":\"%s\"}\n", mac);
    wr = write(fd, req, (size_t)n);
    close(fd);
    return wr == n;
}

static int handshake_ack_raw(void)
{
    int fd;
    struct sockaddr_un a;
    socklen_t alen;
    char buf[256];
    ssize_t n;
    const char *req = "{\"cmd\":\"connect\",\"address\":\"" MAC_A "\"}\n";
    int i;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    i = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "mf-gatt/hci0");
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)i);
    if (connect(fd, (struct sockaddr *)&a, alen) != 0)
    {
        close(fd);
        return 0;
    }
    if (write(fd, req, strlen(req)) < 0)
    {
        close(fd);
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    {
        const char *ok = strstr(buf, "\"type\":\"ok\"");
        const char *nf = strstr(buf, "\"type\":\"notify\"");
        return ok && strstr(buf, "\"cmd\":\"connect\"") && (!nf || ok < nf);
    }
}

int main(int argc, char **argv)
{
    const char *fake_bin, *so_path;
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    const mf_plugin_ops_t *ops = NULL;
    size_t n;
    void *a, *b;
    char spec[256], json[4096], err[96];
    double va, vb;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s fake_gatt libmf_battery_jk.so\n", argv[0]);
        return 2;
    }
    fake_bin = argv[1];
    so_path = argv[2];
    spawn_fake(fake_bin);

    CHECK(handshake_ack_raw(), "handshake ACK before notify");

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
    CHECK(n == 1 && ops, "one ops table");
    CHECK(ops && strcmp(ops->kind, "battery") == 0, "kind battery");
    CHECK(ops && strcmp(ops->driver, "jk") == 0, "driver jk");
    CHECK(ops && (ops->caps(NULL) & MF_CAP_ACTION_SWITCH), "cap switch");

    snprintf(spec, sizeof(spec),
             "{\"name\":\"jk-a\",\"kind\":\"battery\",\"driver\":\"jk\","
             "\"ble\":{\"address\":\"%s\",\"adapter\":\"hci0\"},"
             "\"poll_interval_s\":1.0}", MAC_A);
    a = ops->open(spec, err, sizeof(err));
    CHECK(a != NULL, "open MAC A");
    snprintf(spec, sizeof(spec),
             "{\"name\":\"jk-b\",\"ble\":{\"address\":\"%s\",\"adapter\":\"hci0\"}}",
             MAC_B);
    b = ops->open(spec, err, sizeof(err));
    CHECK(b != NULL, "open MAC B");
    CHECK(ops->fd(a) != ops->fd(b) || ops->fd(a) < 0, "distinct client fds");

    CHECK(drive(ops, a, 3000), "MAC A first frame");
    CHECK(drive(ops, b, 3000), "MAC B first frame");
    CHECK(ops->get_reading(a, json, sizeof(json)) == 0, "read A");
    va = json_num(json, "pack_voltage_v");
    CHECK(va > 52.7 && va < 52.9, "MAC A pack ~52.8 V");
    CHECK(json_true(json, "charge_mosfet_on"), "A charge on");
    CHECK(strstr(json, "temperatures_c") != NULL, "A temps array");
    CHECK(strstr(json, "\"MOS\"") != NULL, "A MOS label");
    CHECK(strstr(json, "\"T1\"") != NULL, "A T1 label");
    CHECK(strstr(json, "remaining_capacity_ah") != NULL, "A remaining Ah");
    CHECK(strstr(json, "\"balancing\"") != NULL, "A cell balancing flag");
    CHECK(ops->get_reading(b, json, sizeof(json)) == 0, "read B");
    vb = json_num(json, "pack_voltage_v");
    CHECK(vb > 54.3 && vb < 54.5, "MAC B pack ~54.4 V");
    CHECK(va != vb, "distinct voltages");
    CHECK(json_true(json, "charge_mosfet_on"), "B charge on");

    CHECK(send_drop(MAC_A), "inject BLE drop A");
    drive(ops, a, 400);
    CHECK(ops->get_reading(a, json, sizeof(json)) != 0, "A offline after drop");
    CHECK(ops->get_reading(b, json, sizeof(json)) == 0, "B still live after A drop");
    CHECK(drive(ops, a, 3000), "A reconnects after drop");
    CHECK(ops->get_reading(a, json, sizeof(json)) == 0, "read A after reconnect");
    va = json_num(json, "pack_voltage_v");
    CHECK(va > 52.7 && va < 52.9, "MAC A pack after reconnect");

    CHECK(ops->action(a, "set_switch",
                      "{\"key\":\"charge\",\"value\":false}",
                      err, sizeof(err)) == MF_OK, "SET charge off A");
    drive_both(ops, a, b, 800);
    CHECK(ops->get_reading(a, json, sizeof(json)) == 0, "read A after SET");
    CHECK(!json_true(json, "charge_mosfet_on"), "A charge off after SET");
    CHECK(ops->get_reading(b, json, sizeof(json)) == 0, "read B after SET A");
    CHECK(json_true(json, "charge_mosfet_on"), "B charge still on (SET isolation)");

    CHECK(ops->get_settings(a, json, sizeof(json)) == 0,
          "settings A before trigger write");
    CHECK(json_num(json, "balance_trigger_v") > 0.009 &&
          json_num(json, "balance_trigger_v") < 0.011,
          "A trigger from frame 0.010");
    CHECK(json_num(json, "start_balance_v") > 2.99 &&
          json_num(json, "start_balance_v") < 3.01,
          "A start from frame 3.000");

    CHECK(ops->action(a, "set_balance_trigger", "{\"volts\":0.030}",
                      err, sizeof(err)) == MF_OK, "trigger 0.030 A");
    CHECK(ops->action(b, "set_balance_trigger", "{\"volts\":2.0}",
                      err, sizeof(err)) == MF_OK, "trigger 2.0 B clamps");
    CHECK(ops->action(a, "set_start_balance", "{\"volts\":3.300}",
                      err, sizeof(err)) == MF_OK, "start 3.300 A");
    CHECK(ops->action(b, "set_start_balance", "{\"volts\":0.5}",
                      err, sizeof(err)) == MF_OK, "start 0.5 B clamps");
    drive_both(ops, a, b, 400);
    CHECK(ops->get_settings(a, json, sizeof(json)) == 0, "settings A");
    CHECK(json_num(json, "balance_trigger_v") > 0.029 &&
          json_num(json, "balance_trigger_v") < 0.031, "A trigger 0.030");
    CHECK(json_num(json, "start_balance_v") > 3.29 &&
          json_num(json, "start_balance_v") < 3.31, "A start 3.300");
    CHECK(ops->get_settings(b, json, sizeof(json)) == 0, "settings B");
    CHECK(json_num(json, "balance_trigger_v") > 0.99 &&
          json_num(json, "balance_trigger_v") < 1.01, "B trigger clamped 1.0");
    CHECK(json_num(json, "start_balance_v") > 1.19 &&
          json_num(json, "start_balance_v") < 1.21, "B start clamped 1.20");
    CHECK(ops->action(a, "set_balance_trigger", "{}", err, sizeof(err))
          == MF_ERR_INVAL, "trigger missing volts");

    CHECK(ops->get_settings(a, json, sizeof(json)) == 0, "settings A live OVP");
    CHECK(json_num(json, "cell_ovp_v") > 3.64 &&
          json_num(json, "cell_ovp_v") < 3.66, "OVP from device 3.65");
    CHECK(json_num(json, "cell_ovpr_v") > 3.54 &&
          json_num(json, "cell_ovpr_v") < 3.56, "OVPR from device 3.55");
    CHECK(json_num(json, "cell_rcv_v") > 3.59 &&
          json_num(json, "cell_rcv_v") < 3.61, "RCV from device 3.60");
    CHECK(ops->put_settings(a, "{\"cell_rcv_v\":3.58}",
                            err, sizeof(err)) == MF_OK, "PUT RCV 3.58");
    drive_both(ops, a, b, 2000);
    CHECK(ops->get_settings(a, json, sizeof(json)) == 0, "settings A after RCV");
    CHECK(json_num(json, "cell_rcv_v") > 3.575 &&
          json_num(json, "cell_rcv_v") < 3.585, "RCV live 3.58");
    CHECK(ops->put_settings(a, "{\"cell_ovp_v\":3.55,\"cell_ovpr_v\":3.45}",
                            err, sizeof(err)) == MF_OK, "PUT OVP 3.55 OVPR 3.45");
    drive_both(ops, a, b, 2000);
    CHECK(ops->get_settings(a, json, sizeof(json)) == 0, "settings A after OVP");
    CHECK(json_num(json, "cell_ovp_v") > 3.54 &&
          json_num(json, "cell_ovp_v") < 3.56, "OVP live 3.55");
    CHECK(json_num(json, "cell_ovpr_v") > 3.44 &&
          json_num(json, "cell_ovpr_v") < 3.46, "OVPR live 3.45");
    CHECK(ops->get_settings(b, json, sizeof(json)) == 0, "settings B after A OVP");
    CHECK(json_num(json, "cell_ovp_v") > 3.64 &&
          json_num(json, "cell_ovp_v") < 3.66, "B OVP unchanged");

    ops->close(a);
    ops->close(b);
    dlclose(dl);
    stop_fake();
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("jk_plugin: ok\n");
    return 0;
}
