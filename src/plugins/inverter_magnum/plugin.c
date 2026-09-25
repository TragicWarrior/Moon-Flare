/*
 * Magnum Energy inverter network, read from a passive RS-485 tap.
 *
 * kind "inverter", driver "magnum"; one module per tap (an FTDI USB-RS485
 * adapter on a splitter).  It never writes to the bus: the port is opened
 * O_RDONLY.  It only opens stable paths (/dev/serial/by-id/..., or the
 * adapter's serial via usb.serial_id), never /dev/ttyUSBn, and refuses a
 * port this daemon already has open.  See README "Magnum inverters".
 *
 * The protocol work is a C port of pymagnum (src/magnum/, see
 * third_party/pymagnum): Copyright (c) 2018-2026 Charles Godwin
 * <magnum@godwin.ca>, BSD-3-Clause.  This file: moon-flare (MIT).
 */

#include "mf_plugin.h"
#include "mag_decode.h"
#include "mag_frame.h"
#include "mag_json.h"
#include "mag_tty.h"
#include "usb_id.h"

#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PUBLISH_DEFAULT_S 1.0
#define PUBLISH_MIN_S     0.5
#define STALE_DEFAULT_S   5.0           /* offline after this long without
                                           an inverter packet */
#define REOPEN_S          2.0
#define OWNED_RETRY_S     10.0
#define DRAIN_MAX         16384
#define TAP_MAX           60

typedef struct {
    char   uuid[40];
    char   path[256];                   /* configured by-id path, or "" */
    char   serial_id[64];
    int    auto_port;
    char   tap[TAP_MAX + 4];
    double poll_s;                      /* how often the reading is rebuilt */
    double stale_s;
    int    fd;
    char   port[PATH_MAX];              /* the device node while open */
    double opened_at;
    double next_open;
    double last_rx;                     /* 0: nothing since opening */
    mag_frame_t fr;
    mag_state_t st;
    char  *reading;
    int    online;
    double next_publish;
    char   err[256];
} mag_ctx_t;

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_packet(void *arg, mag_pkt_t type, const uint8_t *p, size_t len,
                      double t)
{
    mag_ctx_t *c = arg;

    (void)mag_decode(&c->st, type, p, len, t);
}

/* A string setting: nested ({"usb":{"path":..}}, as config and add requests
 * send it) or dotted ("usb.path"). */
static void setting_str(const cJSON *root, const char *obj, const char *key,
                        char *out, size_t cap)
{
    char dotted[64];
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, obj), key);

    if (!v)
    {
        snprintf(dotted, sizeof(dotted), "%s.%s", obj, key);
        v = cJSON_GetObjectItemCaseSensitive(root, dotted);
    }
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(out, cap, "%s", v->valuestring);
}

static int setting_bool(const cJSON *root, const char *obj, const char *key,
                        int dflt)
{
    char dotted[64];
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, obj), key);

    if (!v)
    {
        snprintf(dotted, sizeof(dotted), "%s.%s", obj, key);
        v = cJSON_GetObjectItemCaseSensitive(root, dotted);
    }
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring)
        return strcmp(v->valuestring, "true") == 0 ||
               strcmp(v->valuestring, "1") == 0;
    return dflt;
}

static double number(const cJSON *v, double dflt)
{
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
        return atof(v->valuestring);
    return dflt;
}

static void *mag_open(const char *spec_json, char *err, size_t errsz)
{
    cJSON *spec = spec_json ? cJSON_Parse(spec_json) : NULL;
    mag_ctx_t *c;
    const char *env;

    if (!cJSON_IsObject(spec))
    {
        cJSON_Delete(spec);
        snprintf(err, errsz, "bad module config");
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
    {
        cJSON_Delete(spec);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    setting_str(spec, "usb", "path", c->path, sizeof(c->path));
    setting_str(spec, "usb", "serial_id", c->serial_id, sizeof(c->serial_id));
    setting_str(spec, "magnum", "tap", c->tap, sizeof(c->tap));
    c->auto_port = setting_bool(spec, "usb", "auto_port", 1);
    {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(spec, "uuid");

        if (cJSON_IsString(u))
            snprintf(c->uuid, sizeof(c->uuid), "%s", u->valuestring);
    }
    c->poll_s = number(cJSON_GetObjectItemCaseSensitive(spec, "poll_interval_s"),
                       PUBLISH_DEFAULT_S);
    if (c->poll_s < PUBLISH_MIN_S)
        c->poll_s = PUBLISH_MIN_S;
    cJSON_Delete(spec);

    if (mag_tty_path_unstable(c->path))
    {
        snprintf(err, errsz, "use a /dev/serial/by-id path, not %.40s", c->path);
        free(c);
        return NULL;
    }
    if (!c->path[0] && !c->serial_id[0])
    {
        snprintf(err, errsz, "set usb.path (a /dev/serial/by-id/... path) or "
                 "usb.serial_id");
        free(c);
        return NULL;
    }
    if (strlen(c->tap) > TAP_MAX)
        c->tap[TAP_MAX] = '\0';
    c->stale_s = STALE_DEFAULT_S;
    env = getenv("MF_MAGNUM_STALE_S");  /* tests */
    if (env && atof(env) > 0.0)
        c->stale_s = atof(env);
    c->fd = -1;
    mag_frame_init(&c->fr, on_packet, c);
    mag_state_init(&c->st);
    return c;                           /* the port opens in step() */
}

static void mag_close(void *ctx)
{
    mag_ctx_t *c = ctx;

    if (!c)
        return;
    mag_tty_close(c->fd);
    free(c->reading);
    free(c);
}

static int mag_fd(void *ctx)
{
    return ((mag_ctx_t *)ctx)->fd;
}

static unsigned mag_mask(void *ctx)
{
    return ((mag_ctx_t *)ctx)->fd >= 0 ? MF_IO_WANT_READ : 0;
}

/* Does this daemon already have dev open (another module's port)? */
static int open_in_this_process(const char *dev)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *de;
    char link[300], target[PATH_MAX];
    int found = 0;

    if (!d)
        return 0;
    while (!found && (de = readdir(d)) != NULL)
    {
        ssize_t n;

        if (de->d_name[0] == '.')
            continue;
        snprintf(link, sizeof(link), "/proc/self/fd/%s", de->d_name);
        n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0)
            continue;
        target[n] = '\0';
        found = strcmp(target, dev) == 0;
    }
    closedir(d);
    return found;
}

/* The device node to open: the configured path, else (auto-port) the
 * adapter whose serial matches. */
static int resolve_port(mag_ctx_t *c, char *dev, size_t cap)
{
    char real[PATH_MAX];
    const char *src = NULL;
    mf_usb_id_t id;

    if (c->path[0] && access(c->path, F_OK) == 0)
        src = c->path;
    else if (c->serial_id[0] && (c->auto_port || !c->path[0]))
    {
        if (mf_usb_find_by_serial(c->serial_id, NULL, &id) == 0)
            src = id.path;
        else
        {
            snprintf(c->err, sizeof(c->err), "adapter missing: no USB serial %.50s",
                     c->serial_id);
            return -1;
        }
    }
    else
    {
        snprintf(c->err, sizeof(c->err), "adapter missing: %.70s", c->path);
        return -1;
    }
    if (!realpath(src, real))
    {
        snprintf(c->err, sizeof(c->err), "adapter missing: %.50s: %.20s", src,
                 strerror(errno));
        return -1;
    }
    snprintf(dev, cap, "%s", real);
    return 0;
}

static void try_open(mag_ctx_t *c, double now)
{
    char dev[PATH_MAX];
    int fd;

    c->next_open = now + REOPEN_S;
    if (resolve_port(c, dev, sizeof(dev)) != 0)
        return;
    if (open_in_this_process(dev))
    {
        snprintf(c->err, sizeof(c->err), "%.40s is already open (another module "
                 "owns it)", dev);
        c->next_open = now + OWNED_RETRY_S;
        return;
    }
    fd = mag_tty_open(dev, c->err, sizeof(c->err));
    if (fd < 0)
        return;
    c->fd = fd;
    snprintf(c->port, sizeof(c->port), "%s", dev);
    c->opened_at = now;
    c->last_rx = 0;
    mag_frame_reset_stream(&c->fr);
    if (!c->serial_id[0])
    {
        mf_usb_id_t id;

        if (mf_usb_identify(dev, &id) == 0 && id.serial_id[0])
            snprintf(c->serial_id, sizeof(c->serial_id), "%s", id.serial_id);
    }
    c->err[0] = '\0';
}

/* Read what is waiting.  -1 when the adapter has gone. */
static int drain(mag_ctx_t *c, double now)
{
    uint8_t buf[1024];
    size_t total = 0;

    while (total < DRAIN_MAX)
    {
        ssize_t n = read(c->fd, buf, sizeof(buf));

        if (n > 0)
        {
            total += (size_t)n;
            c->last_rx = now;
            mag_frame_feed(&c->fr, buf, (size_t)n, now);
            continue;
        }
        if (n == 0)
            return -1;                  /* hangup (VMIN=1: never "no data") */
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return 0;
}

/* Why there is no current inverter data, with the port open. */
static void offline_reason(mag_ctx_t *c, double now)
{
    const mag_frame_stats_t *st = &c->fr.st;
    const char *what = c->st.inv.seen ? "no inverter packets" : "no inverter yet";

    if (!c->last_rx)
    {
        if (now - c->opened_at < c->stale_s)
            snprintf(c->err, sizeof(c->err), "waiting for bus data on %.60s", c->port);
        else
            snprintf(c->err, sizeof(c->err), "no data on the bus (%.40s): tap or "
                     "ARTR port silent", c->port);
    }
    else if (now - c->last_rx > c->stale_s)
        snprintf(c->err, sizeof(c->err), "no data on the bus for %.0f s (%.40s)",
                 now - c->last_rx, c->port);
    else
        snprintf(c->err, sizeof(c->err), "%s: bus data but none from an inverter "
                 "(%llu%% 0xFF: check A/B wiring)", what,
                 (unsigned long long)(st->bytes_read ?
                                      st->ff_bytes * 100 / st->bytes_read : 0));
}

static mf_step_t mag_step(void *ctx)
{
    mag_ctx_t *c = ctx;
    double now = now_mono();

    if (c->fd < 0 && now >= c->next_open)
        try_open(c, now);
    if (c->fd >= 0 && drain(c, now) != 0)
    {
        snprintf(c->err, sizeof(c->err), "adapter disconnected (%.60s)", c->port);
        mag_tty_close(c->fd);
        c->fd = -1;
        c->next_open = now + REOPEN_S;
    }
    mag_frame_idle(&c->fr, now);
    /* Offline without the port (its error says why), or without a recent
     * inverter packet. */
    if (c->fd < 0 || !c->st.inv.seen || now - c->st.inv.t > c->stale_s)
    {
        if (c->fd >= 0)
            offline_reason(c, now);
        c->online = 0;
        free(c->reading);
        c->reading = NULL;
        return MF_STEP_ERROR;
    }
    c->online = 1;
    c->err[0] = '\0';
    if (!c->reading || now >= c->next_publish)
    {
        char *json = mag_reading_json(&c->st, &c->fr.st, c->tap, c->port, now);

        if (json)
        {
            free(c->reading);
            c->reading = json;
        }
        c->next_publish = now + c->poll_s;
        return MF_STEP_UPDATED;
    }
    return MF_STEP_IDLE;
}

static unsigned mag_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS | MF_CAP_ACTION_REFRESH |
           MF_CAP_AUTO_PORT;
}

static const char *mag_last_error(void *ctx)
{
    mag_ctx_t *c = ctx;

    return c->err[0] ? c->err : NULL;
}

static int mag_get_reading(void *ctx, char *json, size_t cap)
{
    mag_ctx_t *c = ctx;
    size_t n;

    if (!c->online || !c->reading)
        return MF_ERR_OFFLINE;
    n = strlen(c->reading);
    if (n >= cap)
        return MF_ERR_INVAL;
    memcpy(json, c->reading, n + 1);
    return MF_OK;
}

static int mag_get_settings(void *ctx, char *json, size_t cap)
{
    mag_ctx_t *c = ctx;
    cJSON *o = cJSON_CreateObject();
    char *s;
    int rc = MF_ERR_INVAL;

    if (!o)
        return MF_ERR_INVAL;
    cJSON_AddStringToObject(o, "usb.path", c->path);
    cJSON_AddStringToObject(o, "usb.serial_id", c->serial_id);
    cJSON_AddBoolToObject(o, "usb.auto_port", c->auto_port);
    cJSON_AddStringToObject(o, "magnum.tap", c->tap);
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s && strlen(s) < cap)
    {
        memcpy(json, s, strlen(s) + 1);
        rc = MF_OK;
    }
    free(s);
    return rc;
}

/* The label and publish rate change live; the port is the module's
 * identity (changing it reopens the module). */
static int mag_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    mag_ctx_t *c = ctx;
    cJSON *b = json ? cJSON_Parse(json) : NULL;
    const cJSON *tap, *poll;
    double p = c->poll_s;

    if (!cJSON_IsObject(b))
    {
        cJSON_Delete(b);
        snprintf(err, errsz, "settings are not a JSON object");
        return MF_ERR_INVAL;
    }
    tap = cJSON_GetObjectItemCaseSensitive(b, "magnum.tap");
    if (tap && (!cJSON_IsString(tap) || strlen(tap->valuestring) > TAP_MAX))
    {
        cJSON_Delete(b);
        snprintf(err, errsz, "magnum.tap must be text, at most %d characters",
                 TAP_MAX);
        return MF_ERR_INVAL;
    }
    poll = cJSON_GetObjectItemCaseSensitive(b, "poll_interval_s");
    if (poll)
    {
        p = number(poll, -1.0);
        if (p < PUBLISH_MIN_S)
        {
            cJSON_Delete(b);
            snprintf(err, errsz, "poll_interval_s must be at least %.1f",
                     PUBLISH_MIN_S);
            return MF_ERR_INVAL;
        }
    }
    if (tap)
        snprintf(c->tap, sizeof(c->tap), "%s", tap->valuestring);
    c->poll_s = p;
    if (cJSON_GetObjectItemCaseSensitive(b, "usb.auto_port"))
        c->auto_port = setting_bool(b, "usb", "auto_port", c->auto_port);
    cJSON_Delete(b);
    c->next_publish = 0.0;
    return MF_OK;
}

/* "refresh": start the diag counters over. */
static int mag_action(void *ctx, const char *action, const char *json,
                      char *err, size_t errsz)
{
    mag_ctx_t *c = ctx;

    (void)json;
    if (action && strcmp(action, "refresh") == 0)
    {
        mag_frame_reset_stats(&c->fr);
        c->st.other_inverter_packets = 0;
        c->next_publish = 0.0;
        return MF_OK;
    }
    snprintf(err, errsz, "unknown action: %s", action ? action : "");
    return MF_ERR_UNSUPPORTED;
}

static const char *mag_describe(void)
{
    return
        "{\"bus\":\"usb-serial\",\"fields\":["
        "{\"key\":\"poll_interval_s\",\"label\":\"Publish Every\","
        "\"hint\":\"(Seconds)\",\"type\":\"number\",\"default\":1},"
        "{\"key\":\"usb.path\",\"label\":\"USB Path\","
        "\"hint\":\"(by-id path)\",\"type\":\"string\"},"
        "{\"key\":\"usb.serial_id\",\"label\":\"USB Serial\","
        "\"hint\":\"(FTDI id)\",\"type\":\"string\"},"
        "{\"key\":\"usb.auto_port\",\"label\":\"USB Auto Port\","
        "\"hint\":\"(true/false)\",\"type\":\"bool\",\"default\":true},"
        "{\"key\":\"magnum.tap\",\"label\":\"Tap\","
        "\"hint\":\"(inverter)\",\"type\":\"string\"}"
        "],\"capture\":{\"interval_s\":10,\"min_s\":1,\"retention_days\":60,"
        "\"graph\":\"dc_power_w\",\"columns\":{"
        "\"dc_voltage_v\":\"dc_voltage_v\",\"dc_current_a\":\"dc_current_a\","
        "\"dc_power_w\":\"dc_power_w\",\"ac_out_v\":\"ac_out_v\","
        "\"ac_out_a\":\"ac_out_a\",\"ac_in_v\":\"ac_in_v\",\"ac_in_a\":\"ac_in_a\","
        "\"freq_hz\":\"freq_hz\",\"bat_temp_c\":\"bat_temp_c\","
        "\"tfmr_temp_c\":\"tfmr_temp_c\",\"fet_temp_c\":\"fet_temp_c\","
        "\"mode\":\"mode\",\"fault\":\"fault\",\"stackmode\":\"stackmode\","
        "\"mode_text\":{\"path\":\"mode_text\",\"type\":\"text\"}}}}";
}

static const mf_plugin_ops_t g_ops = {
    .abi          = MF_PLUGIN_ABI,
    .ops_size     = sizeof(mf_plugin_ops_t),
    .kind         = "inverter",
    .driver       = "magnum",
    .version      = "0.1.0",
    .open         = mag_open,
    .close        = mag_close,
    .fd           = mag_fd,
    .select_mask  = mag_mask,
    .step         = mag_step,
    .caps         = mag_caps,
    .last_error   = mag_last_error,
    .get_reading  = mag_get_reading,
    .get_settings = mag_get_settings,
    .put_settings = mag_put_settings,
    .action       = mag_action,
    .describe     = mag_describe,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    *out = &g_ops;
    return 1;
}
