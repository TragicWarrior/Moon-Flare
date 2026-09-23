#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "bms_proto.h"
#include "mf_plugin.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static size_t recorded_0x01_payload(uint8_t *b, size_t cap)
{
    const int n = 16, t = 5;
    size_t base = (size_t)(2 * n + 2 * t);
    size_t need = 44 + base;
    int i;
    if (cap < need)
        return 0;
    memset(b, 0, need);
    b[1] = (uint8_t)n;
    for (i = 0; i < n; i++)
    {
        uint16_t mv = (uint16_t)(3300 + i);
        uint8_t hi = (uint8_t)((mv >> 8) & 0x1F);
        uint8_t lo = (uint8_t)(mv & 0xFF);
        if (i == 3)
            hi = (uint8_t)(hi | 0x80);
        b[2 * i + 2] = hi;
        b[2 * i + 3] = lo;
    }
    put_be16(b + 4 + 2 * n, 28760);
    put_be16(b + 8 + 2 * n, 7600);
    put_be16(b + 12 + 2 * n, 20000);
    b[15 + 2 * n] = (uint8_t)t;
    for (i = 0; i < t; i++)
        b[17 + 2 * n + 2 * i] = (uint8_t)(50 + 25 + i);
    put_be16(b + 30 + base, 42);
    put_be16(b + 34 + base, 5321);
    put_be16(b + 38 + base, 9800);
    return need;
}

static int make_pty(int *master, char *slave, size_t slsz)
{
    int m = -1, s = -1;
    if (openpty(&m, &s, slave, NULL, NULL) < 0)
        return -1;
    close(s);
    if (fcntl(m, F_SETFL, O_NONBLOCK) < 0)
    {
        close(m);
        return -1;
    }
    *master = m;
    (void)slsz;
    return 0;
}

static int reply_one(int master)
{
    uint8_t req[16];
    uint8_t tx[BMS_MAX_FRAME];
    uint8_t payload[BMS_MAX_PAYLOAD];
    ssize_t n;
    size_t plen, flen;
    uint8_t addr, cmd;

    n = read(master, req, sizeof(req));
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n < 6 || req[0] != 0x7E)
        return 0;
    addr = req[1];
    cmd = req[2];
    if (cmd == 0x33)
    {
        const char *fw = "BN-HES16S48V100LT52-V1.3.0";
        plen = strlen(fw);
        memcpy(payload, fw, plen);
    } else if (cmd == 0x42)
    {
        const char *bc = "TBI23012900136";
        plen = strlen(bc);
        memcpy(payload, bc, plen);
    } else if (cmd == 0x01)
    {
        plen = recorded_0x01_payload(payload, sizeof(payload));
    }
    else
    {
        return 0;
    }
    flen = bms_build_frame(addr, cmd, payload, plen, tx);
    if (write(master, tx, flen) != (ssize_t)flen)
        return -1;
    return 1;
}

static mf_step_t drive(const mf_plugin_ops_t *ops, void *ctx, int master,
                      int reply, int max_ms)
{
    int waited = 0;
    mf_step_t last = MF_STEP_IDLE;
    while (waited <= max_ms)
    {
        int fd = ops->fd(ctx);
        unsigned mask = ops->select_mask(ctx);
        struct pollfd p[2];
        int np = 0, to = 50;
        if (fd >= 0 && mask)
        {
            p[np].fd = fd;
            p[np].events = 0;
            if (mask & MF_IO_WANT_READ)
                p[np].events = (short)(p[np].events | POLLIN);
            if (mask & MF_IO_WANT_WRITE)
                p[np].events = (short)(p[np].events | POLLOUT);
            np++;
        }
        if (reply && master >= 0)
        {
            p[np].fd = master;
            p[np].events = POLLIN;
            np++;
        }
        if (np)
            (void)poll(p, (nfds_t)np, to);
        else
            usleep((useconds_t)to * 1000);
        if (reply && master >= 0)
            (void)reply_one(master);
        waited += to;
        last = ops->step(ctx);
        if (last == MF_STEP_UPDATED)
            return last;
        if (last == MF_STEP_ERROR && !reply)
            return last;
    }
    return last;
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
    const char *so_path;
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    const mf_plugin_ops_t *ops = NULL;
    size_t n;
    void *ctx, *ctx2;
    char spec[256], spec2[256];
    char json[8192];
    char err[96];
    char slave[128], slave2[128];
    int master = -1, master2 = -1;
    int fd1, fd2;
    mf_step_t st;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s libmf_battery_xd.so\n", argv[0]);
        return 2;
    }
    so_path = argv[1];
    dl = dlopen(so_path, RTLD_NOW);
    if (!dl)
    {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", so_path, dlerror());
        return 1;
    }
    entries = (size_t (*)(const mf_plugin_ops_t **))dlsym(dl, "mf_plugin_entries");
    if (!entries)
    {
        fprintf(stderr, "FAIL: missing mf_plugin_entries\n");
        return 1;
    }
    n = entries(&ops);
    CHECK(n == 1 && ops, "one ops table");
    CHECK(ops && strcmp(ops->kind, "battery") == 0, "kind battery");
    CHECK(ops && strcmp(ops->driver, "xd") == 0, "driver xd");
    CHECK(ops && (ops->caps(NULL) & MF_CAP_READ), "cap read");
    CHECK(ops && (ops->caps(NULL) & MF_CAP_PROBE), "cap probe");
    CHECK(ops && (ops->caps(NULL) & MF_CAP_AUTO_PORT), "cap auto_port");

    if (make_pty(&master, slave, sizeof(slave)) != 0)
    {
        fprintf(stderr, "FAIL: openpty: %s\n", strerror(errno));
        return 1;
    }
    if (make_pty(&master2, slave2, sizeof(slave2)) != 0)
    {
        fprintf(stderr, "FAIL: openpty 2: %s\n", strerror(errno));
        return 1;
    }

    snprintf(spec, sizeof(spec),
             "{\"name\":\"pack-xd\",\"kind\":\"battery\",\"driver\":\"xd\","
             "\"poll_interval_s\":0.5,\"usb\":{\"path\":\"%s\",\"baud\":9600,"
             "\"addr\":1,\"auto_port\":false}}",
             slave);
    err[0] = '\0';
    ctx = ops->open(spec, err, sizeof(err));
    CHECK(ctx != NULL, "open ctx");
    fd1 = ops->fd(ctx);
    CHECK(fd1 >= 0, "open got fd");

    snprintf(spec2, sizeof(spec2),
             "{\"usb\":{\"path\":\"%s\",\"baud\":9600,\"addr\":1}}", slave2);
    ctx2 = ops->open(spec2, err, sizeof(err));
    CHECK(ctx2 != NULL, "second open ctx");
    fd2 = ops->fd(ctx2);
    CHECK(fd2 >= 0, "second open got fd");
    CHECK(fd1 != fd2, "two open() do not share fd");

    ops->get_settings(ctx, json, sizeof(json));
    CHECK(strstr(json, slave) != NULL, "settings has usb.path");
    CHECK(strstr(json, "\"usb.baud\":9600") != NULL, "settings baud");
    CHECK(strstr(json, "\"usb.addr\":1") != NULL, "settings addr");
    CHECK(strstr(json, "auto_port") != NULL, "settings auto_port");

    CHECK(ops->put_settings(ctx, "{\"poll_interval_s\":0.1}", err, sizeof(err))
          == MF_ERR_INVAL, "poll below 0.5 → INVAL");
    CHECK(ops->put_settings(ctx, "{\"poll_interval_s\":1.0}", err, sizeof(err))
          == MF_OK, "poll 1.0 ok");
    CHECK(ops->action(ctx, "set_switch", "{\"key\":\"charge\",\"value\":false}",
                      err, sizeof(err)) == MF_ERR_UNSUPPORTED,
          "no MOSFET/0x05 writes");

    st = drive(ops, ctx, master, 1, 4000);
    CHECK(st == MF_STEP_UPDATED, "recorded 0x01 via PTY → UPDATED");
    json[0] = '\0';
    CHECK(ops->get_reading(ctx, json, sizeof(json)) == 0, "get_reading");
    CHECK(json_num(json, "pack_voltage_v") > 53.20 &&
          json_num(json, "pack_voltage_v") < 53.22, "reading pack 53.21");
    CHECK(json_num(json, "soc_pct") > 75.99 && json_num(json, "soc_pct") < 76.01,
          "reading soc 76");
    CHECK(json_num(json, "cell_count") == 16.0, "reading 16 cells");
    CHECK(strstr(json, "\"cells\"") != NULL, "cells array for TUI");
    CHECK(strstr(json, "cell_voltages_v") != NULL, "cell_voltages_v");
    CHECK(strstr(json, "BN-HES16S48V100LT52") != NULL, "firmware from 0x33");
    CHECK(strstr(json, "TBI23012900136") != NULL, "barcode from 0x42");
    CHECK(strstr(json, "charge_mosfet_on") == NULL, "XD omits MOSFET keys");

    ops->close(ctx);
    ops->close(ctx2);

    /* Silent PTY: open, never reply, tick until ERROR. */
    {
        char sl[128];
        int m = -1;
        void *silent;
        if (make_pty(&m, sl, sizeof(sl)) != 0)
        {
            fprintf(stderr, "FAIL: silent openpty\n");
            g_fail++;
        }
        else
        {
            snprintf(spec, sizeof(spec),
                     "{\"usb\":{\"path\":\"%s\",\"baud\":9600,\"addr\":1},"
                     "\"poll_interval_s\":0.5}", sl);
            silent = ops->open(spec, err, sizeof(err));
            CHECK(silent != NULL, "silent open ctx");
            st = drive(ops, silent, m, 0, 4000);
            CHECK(st == MF_STEP_ERROR, "silent PTY → ERROR via tick");
            CHECK(ops->last_error(silent) && ops->last_error(silent)[0],
                  "silent last_error set");
            ops->close(silent);
            close(m);
        }
    }

    close(master);
    close(master2);
    dlclose(dl);
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("xd_plugin: ok\n");
    return 0;
}
