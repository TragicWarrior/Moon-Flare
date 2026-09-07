/*
 * libmf_charger_classic.so — non-blocking Modbus TCP FC3, keep-alive.
 * Phase 1 read-only. No libmodbus. Probe: last-known then ≤8-wide TCP.
 */

#include "classic_codec.h"
#include "mf_plugin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int  classic_probe_start(const char *args_json, void **job, char *err, size_t errsz);
mf_step_t classic_probe_step(void *job);
unsigned classic_probe_select_mask(void *job);
void classic_probe_prepare_fds(void *job, fd_set *r, fd_set *w, int *maxfd);
int  classic_probe_result(void *job, char *json, size_t cap);
void classic_probe_close(void *job);

#define REG_START     4100
#define REG_QTY       113          /* 4100..4212 inclusive */
#define REG_IMAGE     4220
#define IO_TIMEOUT_S  1.5
#define RECONNECT_S   30.0
#define POLL_DEFAULT  5.0
#define POLL_MIN      1.0
#define RX_CAP        512
#define TX_CAP        16

enum {
    ST_DISCONNECTED = 0,
    ST_CONNECTING,
    ST_HOLD,           /* TCP up, waiting for next_poll */
    ST_SEND,
    ST_RECV
};

typedef struct {
    char     ip[64];
    int      port;
    int      unit_cfg;             /* configured */
    int      unit_try[3];
    int      ntry;
    int      itry;
    int      unit_ok;              /* last successful, or 0 */
    double   poll_interval_s;

    int      fd;
    unsigned select_mask;
    int      state;
    double   io_deadline;
    double   last_ok;
    double   next_poll;
    int      have_data;
    int      ever_ok;

    uint8_t  tx[TX_CAP];
    size_t   tx_len, tx_off;
    uint8_t  rx[RX_CAP];
    size_t   rx_len;
    uint16_t tid;

    uint16_t        regs[REG_IMAGE];
    classic_data_t  data;
    char            err[96];
} classic_ctx_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static const char *json_find_key(const char *js, const char *key)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || !js)
        return NULL;
    return strstr(js, pat);
}

static int json_int(const char *js, const char *key, int def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    return atoi(p + 1);
}

static double json_double(const char *js, const char *key, double def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    return atof(p + 1);
}

/* Device JSON nests ip/port/unit_id under "modbus". Fall back to the
 * whole document so a flat spec still works. */
static const char *json_modbus_scope(const char *js)
{
    const char *p = json_find_key(js, "modbus");
    return p ? p : (js ? js : "");
}

static void json_str(const char *js, const char *key, char *dst, size_t dstsz)
{
    const char *p = json_find_key(js, key);
    const char *q;
    size_t n;
    dst[0] = '\0';
    if (!p || dstsz == 0)
        return;
    p = strchr(p, ':');
    if (!p)
        return;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return;
    p++;
    q = strchr(p, '"');
    if (!q)
        return;
    n = (size_t)(q - p);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static void sock_close(classic_ctx_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->select_mask = 0;
    c->state = ST_DISCONNECTED;
    c->tx_len = c->tx_off = 0;
    c->rx_len = 0;
}

static void build_unit_list(classic_ctx_t *c)
{
    int seen10 = 0, seen1 = 0;
    c->ntry = 0;
    if (c->unit_cfg > 0) {
        c->unit_try[c->ntry++] = c->unit_cfg;
        if (c->unit_cfg == 10)
            seen10 = 1;
        if (c->unit_cfg == 1)
            seen1 = 1;
    }
    if (!seen10)
        c->unit_try[c->ntry++] = 10;
    if (!seen1)
        c->unit_try[c->ntry++] = 1;
    c->itry = 0;
}

static int current_unit(const classic_ctx_t *c)
{
    if (c->itry < 0 || c->itry >= c->ntry)
        return 10;
    return c->unit_try[c->itry];
}

static int start_connect(classic_ctx_t *c)
{
    struct sockaddr_in a;
    int one = 1;
    int fd;

    sock_close(c);
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(c->err, sizeof(c->err), "socket: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)c->port);
    if (inet_pton(AF_INET, c->ip, &a.sin_addr) != 1) {
        snprintf(c->err, sizeof(c->err), "bad ip %s", c->ip);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 &&
        errno != EINPROGRESS) {
        snprintf(c->err, sizeof(c->err), "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    c->fd = fd;
    c->state = ST_CONNECTING;
    c->select_mask = MF_IO_WANT_WRITE;
    c->io_deadline = mono_now() + IO_TIMEOUT_S;
    c->tx_off = c->tx_len = 0;
    c->rx_len = 0;
    return 0;
}

static void build_fc3(classic_ctx_t *c)
{
    uint8_t unit = (uint8_t)current_unit(c);
    c->tid++;
    if (c->tid == 0)
        c->tid = 1;
    put_be16(c->tx + 0, c->tid);
    put_be16(c->tx + 2, 0);
    put_be16(c->tx + 4, 6);
    c->tx[6] = unit;
    c->tx[7] = 0x03;
    put_be16(c->tx + 8, (uint16_t)REG_START);
    put_be16(c->tx + 10, (uint16_t)REG_QTY);
    c->tx_len = 12;
    c->tx_off = 0;
    c->rx_len = 0;
    c->state = ST_SEND;
    c->select_mask = MF_IO_WANT_WRITE;
    c->io_deadline = mono_now() + IO_TIMEOUT_S;
}

static int parse_fc3(classic_ctx_t *c)
{
    uint16_t length, i;
    uint8_t unit, fc, nbytes;
    classic_registers_t input;
    classic_result_t rc;

    if (c->rx_len < 6)
        return 0; /* need more */
    length = be16(c->rx + 4);
    if (length < 3 || (size_t)length + 6 > sizeof(c->rx))
        return -1;
    if (c->rx_len < 6u + (size_t)length)
        return 0;
    unit = c->rx[6];
    fc = c->rx[7];
    if (fc & 0x80u)
        return -2; /* exception: try next unit */
    if (fc != 0x03)
        return -1;
    nbytes = c->rx[8];
    if (nbytes != (uint8_t)(REG_QTY * 2) || 6u + (size_t)length < 9u + nbytes)
        return -1;
    (void)unit;
    memset(c->regs, 0, sizeof(c->regs));
    for (i = 0; i < (uint16_t)REG_QTY; i++)
        c->regs[REG_START + i] = be16(c->rx + 9 + (size_t)i * 2u);
    input.regs = c->regs;
    input.reg_count = REG_IMAGE;
    rc = classic_decode(&input, &c->data);
    if (rc != CLASSIC_OK) {
        snprintf(c->err, sizeof(c->err), "decode %d", (int)rc);
        return -1;
    }
    c->unit_ok = current_unit(c);
    c->have_data = 1;
    c->ever_ok = 1;
    c->last_ok = mono_now();
    c->next_poll = c->last_ok + c->poll_interval_s;
    c->err[0] = '\0';
    return 1;
}

static void next_unit_or_reconnect(classic_ctx_t *c)
{
    sock_close(c);
    c->itry++;
    if (c->itry >= c->ntry) {
        c->itry = 0;
        snprintf(c->err, sizeof(c->err), "no unit id answered");
        return;
    }
    (void)start_connect(c);
}

static int connect_ready(classic_ctx_t *c)
{
    struct pollfd p;
    int err = 0;
    socklen_t elen = (socklen_t)sizeof(err);

    p.fd = c->fd;
    p.events = POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLOUT))
        return 0;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
        snprintf(c->err, sizeof(c->err), "connect: %s",
                 err ? strerror(err) : "failed");
        return -1;
    }
    return 1;
}

static void prefer_unit_ok(classic_ctx_t *c)
{
    int i;
    if (!c->unit_ok)
        return;
    for (i = 0; i < c->ntry; i++) {
        if (c->unit_try[i] == c->unit_ok) {
            c->itry = i;
            return;
        }
    }
}

static mf_step_t pump(classic_ctx_t *c)
{
    double now = mono_now();

    if (c->state == ST_HOLD) {
        if (now < c->next_poll)
            return MF_STEP_IDLE;
        if ((now - c->last_ok) >= RECONNECT_S) {
            sock_close(c);
            c->itry = 0;
            if (start_connect(c) != 0)
                return MF_STEP_ERROR;
            return MF_STEP_IDLE;
        }
        prefer_unit_ok(c);
        build_fc3(c);
    }

    if (c->state == ST_DISCONNECTED) {
        c->itry = 0;
        if (start_connect(c) != 0)
            return MF_STEP_ERROR;
        return MF_STEP_IDLE;
    }

    if (c->state != ST_HOLD && now > c->io_deadline) {
        snprintf(c->err, sizeof(c->err), "io timeout unit %d", current_unit(c));
        next_unit_or_reconnect(c);
        return MF_STEP_ERROR;
    }

    if (c->state == ST_CONNECTING) {
        int cr = connect_ready(c);
        if (cr == 0)
            return MF_STEP_IDLE;
        if (cr < 0) {
            next_unit_or_reconnect(c);
            return MF_STEP_ERROR;
        }
        prefer_unit_ok(c);
        build_fc3(c);
    }

    if (c->state == ST_SEND) {
        while (c->tx_off < c->tx_len) {
            ssize_t n = send(c->fd, c->tx + c->tx_off, c->tx_len - c->tx_off,
                             MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    return MF_STEP_IDLE;
                snprintf(c->err, sizeof(c->err), "send: %s", strerror(errno));
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            if (n == 0) {
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            c->tx_off += (size_t)n;
        }
        c->state = ST_RECV;
        c->select_mask = MF_IO_WANT_READ;
        c->rx_len = 0;
        c->io_deadline = now + IO_TIMEOUT_S;
    }

    if (c->state == ST_RECV) {
        for (;;) {
            ssize_t n;
            int pr;
            if (c->rx_len >= sizeof(c->rx)) {
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    return MF_STEP_IDLE;
                snprintf(c->err, sizeof(c->err), "recv: %s", strerror(errno));
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            if (n == 0) {
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            c->rx_len += (size_t)n;
            pr = parse_fc3(c);
            if (pr == 0)
                continue;
            if (pr < 0) {
                if (pr == -2)
                    snprintf(c->err, sizeof(c->err), "modbus exception");
                next_unit_or_reconnect(c);
                return MF_STEP_ERROR;
            }
            c->state = ST_HOLD;
            c->select_mask = 0;
            c->tx_len = c->tx_off = 0;
            c->rx_len = 0;
            c->io_deadline = c->next_poll + RECONNECT_S;
            return MF_STEP_UPDATED;
        }
    }

    return MF_STEP_IDLE;
}

static void *classic_open(const char *spec_json, char *err, size_t errsz)
{
    classic_ctx_t *c = calloc(1, sizeof(*c));
    double iv;
    if (!c) {
        if (err && errsz)
            snprintf(err, errsz, "oom");
        return NULL;
    }
    c->fd = -1;
    {
        const char *mb = json_modbus_scope(spec_json);
        json_str(mb, "ip", c->ip, sizeof(c->ip));
        c->port = json_int(mb, "port", 502);
        c->unit_cfg = json_int(mb, "unit_id", 10);
    }
    if (!c->ip[0])
        snprintf(c->ip, sizeof(c->ip), "127.0.0.1");
    if (c->port <= 0 || c->port > 65535)
        c->port = 502;
    iv = json_double(spec_json, "poll_interval_s", 0.0);
    if (iv <= 0.0)
        c->poll_interval_s = POLL_DEFAULT;
    else
        c->poll_interval_s = iv;
    if (c->poll_interval_s < POLL_MIN)
        c->poll_interval_s = POLL_MIN;
    build_unit_list(c);
    c->next_poll = 0; /* poll immediately */
    if (start_connect(c) != 0) {
        if (err && errsz)
            snprintf(err, errsz, "%s", c->err);
        /* still return ctx: step will retry */
    }
    return c;
}

static void classic_close(void *v)
{
    classic_ctx_t *c = v;
    if (!c)
        return;
    sock_close(c);
    free(c);
}

static int classic_fd(void *v)
{
    classic_ctx_t *c = v;
    return c ? c->fd : -1;
}

static unsigned classic_mask(void *v)
{
    classic_ctx_t *c = v;
    return c ? c->select_mask : 0;
}

static mf_step_t classic_step(void *v)
{
    classic_ctx_t *c = v;
    if (!c)
        return MF_STEP_ERROR;
    return pump(c);
}

static unsigned classic_caps(void *v)
{
    (void)v;
    return MF_CAP_READ | MF_CAP_PROBE | MF_CAP_AUTO_NET;
}

static const char *classic_last_error(void *v)
{
    classic_ctx_t *c = v;
    return (c && c->err[0]) ? c->err : "";
}

static int classic_get_reading(void *v, char *json, size_t cap)
{
    classic_ctx_t *c = v;
    const classic_data_t *d;
    if (!c || !json || cap == 0)
        return -1;
    if (!c->have_data) {
        snprintf(json, cap, "{}");
        return 0;
    }
    d = &c->data;
    snprintf(json, cap,
             "{\"battery_voltage_v\":%.2f,\"battery_current_a\":%.2f,"
             "\"charging_watts\":%u,\"kwh_today\":%.2f,\"ah_today\":%.1f,"
             "\"lifetime_kwh\":%u,\"charge_stage\":\"%s\","
             "\"classic_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"unit_device_id\":%u,\"unit_id\":%d}",
             (double)d->battery_voltage_v, (double)d->ibatt_a,
             (unsigned)d->watts, (double)d->kwh_today, (double)d->ah_today,
             (unsigned)d->lifetime_kwh, d->charge_stage_name,
             d->unit_mac[0], d->unit_mac[1], d->unit_mac[2],
             d->unit_mac[3], d->unit_mac[4], d->unit_mac[5],
             (unsigned)d->device_id, c->unit_ok ? c->unit_ok : current_unit(c));
    return 0;
}

static int classic_get_settings(void *v, char *json, size_t cap)
{
    classic_ctx_t *c = v;
    if (!c || !json || cap == 0)
        return -1;
    snprintf(json, cap,
             "{\"modbus.ip\":\"%s\",\"modbus.port\":%d,\"modbus.unit_id\":%d,"
             "\"poll_interval_s\":%.1f}",
             c->ip, c->port, c->unit_cfg, c->poll_interval_s);
    return 0;
}

static int classic_put_settings(void *v, const char *json, char *err, size_t errsz)
{
    (void)v;
    (void)json;
    if (err && errsz)
        snprintf(err, errsz, "read-only");
    return MF_ERR_UNSUPPORTED;
}

static int classic_action(void *v, const char *action, const char *json,
                          char *err, size_t errsz)
{
    (void)v;
    (void)action;
    (void)json;
    if (err && errsz)
        snprintf(err, errsz, "read-only");
    return MF_ERR_UNSUPPORTED;
}

static const mf_plugin_ops_t g_ops = {
    .abi = MF_PLUGIN_ABI,
    .ops_size = sizeof(mf_plugin_ops_t),
    .kind = "charger",
    .driver = "classic",
    .version = "0.1.0",
    .open = classic_open,
    .close = classic_close,
    .fd = classic_fd,
    .select_mask = classic_mask,
    .prepare_fds = NULL,
    .step = classic_step,
    .caps = classic_caps,
    .last_error = classic_last_error,
    .get_reading = classic_get_reading,
    .get_settings = classic_get_settings,
    .put_settings = classic_put_settings,
    .action = classic_action,
    .probe_start = classic_probe_start,
    .probe_step = classic_probe_step,
    .probe_select_mask = classic_probe_select_mask,
    .probe_prepare_fds = classic_probe_prepare_fds,
    .probe_result = classic_probe_result,
    .probe_close = classic_probe_close,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = &g_ops;
    return 1;
}
