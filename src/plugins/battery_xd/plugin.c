/*
 * libmf_battery_xd.so — non-blocking RS485 wrap of bms_txrx_*.
 * Prelude 0x33 / 0x42 then poll 0x01. No 0x05/0x06 writes.
 * USB auto-port via usb_id on open/error only (not every poll).
 */

#include "bms_proto.h"
#include "mf_plugin.h"
#include "usb_id.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_DEFAULT 2.0
#define POLL_MIN     0.5
#define IO_TIMEOUT_S 1.5
#define TX_SETTLE_S  0.25

enum {
    CMD_FW = 0,
    CMD_BARCODE,
    CMD_POLL
};

typedef struct {
    char     uuid[40];
    char     path[128];
    char     serial_id[64];
    char     by_id[256];
    int      baud;
    int      addr;
    int      auto_port;
    double   poll_interval_s;

    int      fd;
    unsigned select_mask;
    int      io_active;
    int      cmd_phase;
    double   next_poll;
    double   reopen_at;

    bms_txrx_state_t io;
    bms_realtime_t   rt;
    int              have_data;
    char             firmware[64];
    char             barcode[64];
    char             err[96];
} xd_ctx_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *json_find_key(const char *js, const char *key)
{
    char pat[64];
    int n;
    if (!js || !key)
        return NULL;
    n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0)
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

static int json_bool(const char *js, const char *key, int def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "true", 4) == 0)
        return 1;
    if (strncmp(p, "false", 5) == 0)
        return 0;
    return def;
}

static const char *json_usb_scope(const char *js)
{
    const char *p = json_find_key(js, "usb");
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

static void set_err(xd_ctx_t *c, const char *msg)
{
    if (!c)
        return;
    snprintf(c->err, sizeof(c->err), "%s", msg ? msg : "");
}

static void copy_ascii(char *dst, size_t cap, const uint8_t *body, int n)
{
    int len = 0;
    if (!dst || cap == 0)
        return;
    while (len < n && body[len] != 0)
        len++;
    while (len > 0 && (body[len - 1] == ' ' || body[len - 1] == '\t' ||
                       body[len - 1] == '\r' || body[len - 1] == '\n'))
        len--;
    if ((size_t)len + 1 > cap)
        len = (int)cap - 1;
    memcpy(dst, body, (size_t)len);
    dst[len] = '\0';
}

static uint8_t cmd_for_phase(int phase)
{
    if (phase == CMD_FW)
        return 0x33;
    if (phase == CMD_BARCODE)
        return 0x42;
    return 0x01;
}

static int try_open_fd(xd_ctx_t *c)
{
    int fd;
    if (!c->path[0]) {
        set_err(c, "usb.path required");
        return -1;
    }
    fd = bms_serial_open_nonblock(c->path, c->baud);
    if (fd < 0) {
        snprintf(c->err, sizeof(c->err), "open failed: %s", strerror(errno));
        return -1;
    }
    c->fd = fd;
    c->err[0] = '\0';
    return 0;
}

static int apply_usb_policy(xd_ctx_t *c, int opened)
{
    mf_usb_want_t w;
    mf_usb_id_t got;
    int rc;

    memset(&w, 0, sizeof(w));
    snprintf(w.path, sizeof(w.path), "%s", c->path);
    snprintf(w.serial_id, sizeof(w.serial_id), "%s", c->serial_id);
    snprintf(w.by_id, sizeof(w.by_id), "%s", c->by_id);
    w.auto_port = c->auto_port;
    w.path_ok = opened ? 1 : 0;
    rc = mf_usb_resolve(c->uuid, &w, &got);
    if (rc == MF_USB_LEARN || rc == MF_USB_USE) {
        if (got.serial_id[0])
            snprintf(c->serial_id, sizeof(c->serial_id), "%s", got.serial_id);
        if (got.by_id[0])
            snprintf(c->by_id, sizeof(c->by_id), "%s", got.by_id);
        return 0;
    }
    if (rc == MF_USB_RELOCATE) {
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1;
            c->io_active = 0;
        }
        snprintf(c->path, sizeof(c->path), "%s", got.path);
        if (got.serial_id[0])
            snprintf(c->serial_id, sizeof(c->serial_id), "%s", got.serial_id);
        if (got.by_id[0])
            snprintf(c->by_id, sizeof(c->by_id), "%s", got.by_id);
        return try_open_fd(c);
    }
    if (opened && c->fd >= 0 && (c->serial_id[0] || c->by_id[0])) {
        close(c->fd);
        c->fd = -1;
        c->io_active = 0;
        set_err(c, "usb identity mismatch");
        return -1;
    }
    return opened ? 0 : -1;
}

static void start_cmd(xd_ctx_t *c, int phase)
{
    c->cmd_phase = phase;
    c->io_active = 1;
    bms_txrx_init(&c->io, c->fd, (uint8_t)c->addr, cmd_for_phase(phase),
                  NULL, 0, IO_TIMEOUT_S, TX_SETTLE_S);
}

static void *xd_open(const char *spec_json, char *err, size_t errsz)
{
    xd_ctx_t *c = calloc(1, sizeof(*c));
    const char *usb;
    double iv;
    if (!c) {
        if (err && errsz)
            snprintf(err, errsz, "oom");
        return NULL;
    }
    c->fd = -1;
    c->baud = 9600;
    c->addr = 1;
    c->auto_port = 0;
    c->poll_interval_s = POLL_DEFAULT;
    usb = json_usb_scope(spec_json ? spec_json : "");
    json_str(spec_json ? spec_json : "", "uuid", c->uuid, sizeof(c->uuid));
    json_str(usb, "path", c->path, sizeof(c->path));
    json_str(usb, "serial_id", c->serial_id, sizeof(c->serial_id));
    json_str(usb, "by_id", c->by_id, sizeof(c->by_id));
    c->baud = json_int(usb, "baud", 9600);
    c->addr = json_int(usb, "addr", 1);
    c->auto_port = json_bool(usb, "auto_port", 0);
    if (c->baud <= 0)
        c->baud = 9600;
    if (c->addr <= 0 || c->addr > 255)
        c->addr = 1;
    iv = json_double(spec_json ? spec_json : "", "poll_interval_s", 0.0);
    if (iv > 0.0)
        c->poll_interval_s = iv;
    if (c->poll_interval_s < POLL_MIN)
        c->poll_interval_s = POLL_MIN;
    c->next_poll = 0;
    c->cmd_phase = CMD_FW;
    if (try_open_fd(c) == 0) {
        if (apply_usb_policy(c, 1) != 0) {
            if (err && errsz)
                snprintf(err, errsz, "%s", c->err);
            c->reopen_at = mono_now() + c->poll_interval_s;
        } else {
            start_cmd(c, CMD_FW);
        }
    } else if (apply_usb_policy(c, 0) == 0 && c->fd >= 0) {
        start_cmd(c, CMD_FW);
    } else {
        if (err && errsz)
            snprintf(err, errsz, "%s", c->err);
        c->reopen_at = mono_now() + c->poll_interval_s;
    }
    return c;
}

static void xd_close(void *v)
{
    xd_ctx_t *c = v;
    if (!c)
        return;
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

static int xd_fd(void *v)
{
    xd_ctx_t *c = v;
    return c ? c->fd : -1;
}

static unsigned xd_mask(void *v)
{
    xd_ctx_t *c = v;
    return c ? c->select_mask : 0;
}

static mf_step_t xd_step(void *v)
{
    xd_ctx_t *c = v;
    bms_io_t s;
    double now;
    if (!c)
        return MF_STEP_ERROR;
    now = mono_now();
    c->select_mask = 0;
    if (c->fd < 0) {
        if (now < c->reopen_at)
            return MF_STEP_IDLE;
        if (try_open_fd(c) == 0) {
            if (apply_usb_policy(c, 1) != 0) {
                c->reopen_at = now + c->poll_interval_s;
                return MF_STEP_ERROR;
            }
        } else if (apply_usb_policy(c, 0) != 0 || c->fd < 0) {
            c->reopen_at = now + c->poll_interval_s;
            return MF_STEP_ERROR;
        }
        start_cmd(c, CMD_FW);
    }
    if (!c->io_active) {
        if (now < c->next_poll)
            return MF_STEP_IDLE;
        start_cmd(c, c->cmd_phase);
    }
    s = bms_txrx_step(&c->io);
    if (s == BMS_IO_NEED_READ) {
        c->select_mask = MF_IO_WANT_READ;
        return MF_STEP_IDLE;
    }
    if (s == BMS_IO_NEED_WRITE) {
        c->select_mask = MF_IO_WANT_WRITE;
        return MF_STEP_IDLE;
    }
    if (s == BMS_IO_NEED_TIME)
        return MF_STEP_IDLE;
    c->io_active = 0;
    if (s == BMS_IO_DONE) {
        if (c->cmd_phase == CMD_FW) {
            copy_ascii(c->firmware, sizeof(c->firmware),
                       c->io.body, c->io.body_len);
            c->cmd_phase = CMD_BARCODE;
            c->next_poll = now;
            c->err[0] = '\0';
            return MF_STEP_IDLE;
        }
        if (c->cmd_phase == CMD_BARCODE) {
            copy_ascii(c->barcode, sizeof(c->barcode),
                       c->io.body, c->io.body_len);
            c->cmd_phase = CMD_POLL;
            c->next_poll = now;
            c->err[0] = '\0';
            return MF_STEP_IDLE;
        }
        if (bms_parse_realtime(c->io.body, (size_t)c->io.body_len, &c->rt) != 0) {
            set_err(c, "parse failed");
            c->next_poll = now + c->poll_interval_s;
            return MF_STEP_ERROR;
        }
        c->have_data = 1;
        c->err[0] = '\0';
        c->next_poll = now + c->poll_interval_s;
        return MF_STEP_UPDATED;
    }
    snprintf(c->err, sizeof(c->err), "%s",
             c->io.err[0] ? c->io.err : "io error");
    c->next_poll = now + c->poll_interval_s;
    if (c->cmd_phase == CMD_FW)
        c->cmd_phase = CMD_BARCODE;
    else if (c->cmd_phase == CMD_BARCODE)
        c->cmd_phase = CMD_POLL;
    return MF_STEP_ERROR;
}

static unsigned xd_caps(void *v)
{
    (void)v;
    return MF_CAP_READ | MF_CAP_PROBE | MF_CAP_AUTO_PORT;
}

static const char *xd_last_error(void *v)
{
    xd_ctx_t *c = v;
    return (c && c->err[0]) ? c->err : "";
}

static int js_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*off >= cap)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= cap - *off) {
        *off = cap - 1;
        buf[cap - 1] = '\0';
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

static int xd_get_reading(void *v, char *json, size_t cap)
{
    xd_ctx_t *c = v;
    const bms_realtime_t *r;
    size_t off = 0;
    int i;
    if (!c || !json || cap == 0)
        return -1;
    if (!c->have_data) {
        snprintf(json, cap, "{}");
        return 0;
    }
    r = &c->rt;
    js_append(json, cap, &off,
              "{\"pack_voltage_v\":%.2f,\"current_a\":%.2f,\"soc_pct\":%.2f,"
              "\"soh_pct\":%.2f,\"cell_count\":%u,\"full_capacity_ah\":%.2f,"
              "\"remaining_capacity_ah\":%.2f,\"cycles\":%u,"
              "\"firmware\":\"%s\",\"barcode\":\"%s\",\"cell_voltages_v\":[",
              r->pack_voltage_v, r->current_a, r->soc_pct, r->soh_pct,
              (unsigned)r->cell_count, r->full_capacity_ah,
              r->remaining_capacity_ah, (unsigned)r->cycles,
              c->firmware, c->barcode);
    for (i = 0; i < (int)r->cell_count; i++)
        js_append(json, cap, &off, "%s%.3f", i ? "," : "", r->cell_voltages_v[i]);
    js_append(json, cap, &off, "],\"cell_balancing\":[");
    for (i = 0; i < (int)r->cell_count; i++)
        js_append(json, cap, &off, "%s%s", i ? "," : "",
                  r->cell_balancing[i] ? "true" : "false");
    js_append(json, cap, &off, "],\"cells\":[");
    for (i = 0; i < (int)r->cell_count; i++)
        js_append(json, cap, &off,
                  "%s{\"index\":%d,\"voltage_v\":%.3f,\"balancing\":%s}",
                  i ? "," : "", i + 1, r->cell_voltages_v[i],
                  r->cell_balancing[i] ? "true" : "false");
    js_append(json, cap, &off, "],\"temperatures_c\":[");
    for (i = 0; i < (int)r->temp_count; i++)
        js_append(json, cap, &off, "%s%.1f", i ? "," : "", r->temperatures_c[i]);
    js_append(json, cap, &off, "],\"temp_labels\":[");
    for (i = 0; i < (int)r->temp_count; i++) {
        if (i == 0)
            js_append(json, cap, &off, "\"MOS\"");
        else
            js_append(json, cap, &off, ",\"T%d\"", i);
    }
    js_append(json, cap, &off, "]}");
    return 0;
}

static int xd_get_settings(void *v, char *json, size_t cap)
{
    xd_ctx_t *c = v;
    if (!c || !json || cap == 0)
        return -1;
    snprintf(json, cap,
             "{\"usb.path\":\"%s\",\"usb.serial_id\":\"%s\",\"usb.by_id\":\"%s\","
             "\"usb.baud\":%d,\"usb.addr\":%d,\"usb.auto_port\":%s,"
             "\"poll_interval_s\":%.1f}",
             c->path, c->serial_id, c->by_id, c->baud, c->addr,
             c->auto_port ? "true" : "false", c->poll_interval_s);
    return 0;
}

static int xd_put_settings(void *v, const char *json, char *err, size_t errsz)
{
    xd_ctx_t *c = v;
    const char *usb;
    int baud, addr, ap;
    double iv;
    if (err && errsz)
        err[0] = '\0';
    if (!c)
        return MF_ERR_INVAL;
    if (!json)
        json = "{}";
    usb = json_usb_scope(json);
    iv = json_double(json, "poll_interval_s", -1.0);
    if (iv >= 0.0) {
        if (iv < POLL_MIN) {
            if (err && errsz)
                snprintf(err, errsz, "poll_interval_s below %.1f", POLL_MIN);
            return MF_ERR_INVAL;
        }
        c->poll_interval_s = iv;
    }
    baud = json_int(usb, "baud", -1);
    if (json_find_key(usb, "baud") && baud > 0)
        c->baud = baud;
    addr = json_int(usb, "addr", -1);
    if (json_find_key(usb, "addr") && addr > 0 && addr <= 255)
        c->addr = addr;
    if (json_find_key(usb, "auto_port")) {
        ap = json_bool(usb, "auto_port", c->auto_port);
        c->auto_port = ap;
    }
    return MF_OK;
}

static int xd_action(void *v, const char *action, const char *json,
                     char *err, size_t errsz)
{
    (void)v;
    (void)action;
    (void)json;
    if (err && errsz)
        snprintf(err, errsz, "unsupported");
    return MF_ERR_UNSUPPORTED;
}

static int xd_probe_start(const char *args_json, void **job, char *err, size_t errsz)
{
    (void)args_json;
    (void)job;
    if (err && errsz)
        snprintf(err, errsz, "usb probe is PR-12");
    return -1;
}

static mf_step_t xd_probe_step(void *job)
{
    (void)job;
    return MF_STEP_ERROR;
}

static unsigned xd_probe_mask(void *job)
{
    (void)job;
    return 0;
}

static void xd_probe_prepare(void *job, fd_set *r, fd_set *w, int *maxfd)
{
    (void)job;
    (void)r;
    (void)w;
    (void)maxfd;
}

static int xd_probe_result(void *job, char *json, size_t cap)
{
    (void)job;
    if (json && cap)
        snprintf(json, cap, "{\"status\":\"error\",\"results\":[]}");
    return -1;
}

static void xd_probe_close(void *job)
{
    (void)job;
}

static const mf_plugin_ops_t g_ops = {
    .abi = MF_PLUGIN_ABI,
    .ops_size = sizeof(mf_plugin_ops_t),
    .kind = "battery",
    .driver = "xd",
    .version = "0.1.0",
    .open = xd_open,
    .close = xd_close,
    .fd = xd_fd,
    .select_mask = xd_mask,
    .prepare_fds = NULL,
    .step = xd_step,
    .caps = xd_caps,
    .last_error = xd_last_error,
    .get_reading = xd_get_reading,
    .get_settings = xd_get_settings,
    .put_settings = xd_put_settings,
    .action = xd_action,
    .probe_start = xd_probe_start,
    .probe_step = xd_probe_step,
    .probe_select_mask = xd_probe_mask,
    .probe_prepare_fds = xd_probe_prepare,
    .probe_result = xd_probe_result,
    .probe_close = xd_probe_close,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = &g_ops;
    return 1;
}
