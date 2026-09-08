/*
 * Device slots: live / dying / free. No pt_kill. LIST/GET walk live only.
 * Uniqueness of name among all in_use (including dying).
 */

#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

void mf_log(int prio, const char *fmt, ...);

#define LOG_I(...) mf_log(LOG_INFO,    __VA_ARGS__)
#define LOG_W(...) mf_log(LOG_WARNING, __VA_ARGS__)

typedef struct {
    pt_func_t pt_func;
    int       idx;
} device_env_t;

typedef struct mf_device {
    bool     in_use;
    bool     stop;
    char     uuid[MF_UUID_LEN];
    char     name[32];
    char     kind[16];
    char     driver[16];
    unsigned caps;
    const mf_plugin_ops_t *ops;
    void    *ctx;

    bool     online;
    bool     have_data;
    uint64_t seq;
    double   poll_interval_s;
    char     last_error[96];
    char     reading_json[MF_READING_JSON_SZ];

    pt_thread_t  thr;
    device_env_t env;
} mf_device_t;

static mf_device_t                 g_dev[MF_DEVICE_SLOTS];
static protothread_t               g_pts;
static char                       *g_chan_tick;
static volatile sig_atomic_t      *g_quit;
static const mf_plugin_registry_t *g_reg;

static pt_t device_pt(env_t e_);

static int uuid_generate(char *buf, size_t bufsz)
{
    FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
    if (f) {
        size_t n = fread(buf, 1, 36, f);
        fclose(f);
        if (n >= 36) {
            buf[36] = '\0';
            return 0;
        }
    }
    snprintf(buf, bufsz, "00000000-0000-4000-8000-%012lx",
             (unsigned long)time(NULL));
    buf[36] = '\0';
    return 0;
}

static void fill_info(const mf_device_t *d, mf_devinfo_t *out)
{
    out->uuid = d->uuid;
    out->name = d->name;
    out->kind = d->kind;
    out->driver = d->driver;
    out->online = d->online;
    out->dying = d->in_use && d->stop;
    out->seq = d->seq;
    out->caps = d->caps;
    out->reading_json = d->have_data ? d->reading_json : "{}";
    out->last_error = d->last_error;
}

static int name_taken(const char *name, int skip)
{
    int i;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (i == skip)
            continue;
        if (g_dev[i].in_use && strcmp(g_dev[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static void slot_clear(mf_device_t *d)
{
    if (d->ops && d->ops->close && d->ctx)
        d->ops->close(d->ctx);
    memset(d, 0, sizeof(*d));
}

static void refresh_reading(mf_device_t *d)
{
    char tmp[MF_READING_JSON_SZ];
    int rc;

    if (!d->ops || !d->ops->get_reading || !d->ctx)
        return;
    tmp[0] = '\0';
    rc = d->ops->get_reading(d->ctx, tmp, sizeof(tmp));
    if (rc != 0) {
        d->online = false;
        if (d->ops->last_error && d->ops->last_error(d->ctx))
            snprintf(d->last_error, sizeof(d->last_error), "%s",
                     d->ops->last_error(d->ctx));
        return;
    }
    memcpy(d->reading_json, tmp, sizeof(d->reading_json) - 1);
    d->reading_json[sizeof(d->reading_json) - 1] = '\0';
    d->have_data = true;
    d->online = true;
    d->seq++;
    d->last_error[0] = '\0';
    if (d->ops->caps)
        d->caps = d->ops->caps(d->ctx);
}

static pt_t device_pt(env_t e_)
{
    device_env_t *env = e_;
    mf_device_t *d = &g_dev[env->idx];

    pt_resume(env);
    while ((!g_quit || !*g_quit) && d->in_use && !d->stop) {
        pt_wait(env, g_chan_tick);
        if ((g_quit && *g_quit) || !d->in_use || d->stop)
            break;
        if (d->ops && d->ops->step && d->ctx) {
            mf_step_t st = d->ops->step(d->ctx);
            if (st == MF_STEP_ERROR) {
                d->online = false;
                if (d->ops->last_error && d->ops->last_error(d->ctx))
                    snprintf(d->last_error, sizeof(d->last_error), "%s",
                             d->ops->last_error(d->ctx));
            } else {
                refresh_reading(d);
            }
        }
    }
    if (d->ops && d->ops->close && d->ctx) {
        d->ops->close(d->ctx);
        d->ctx = NULL;
    }
    d->in_use = false;
    d->stop = false;
    return PT_DONE;
}

void mf_devices_init(protothread_t pts, char *chan_tick,
                     volatile sig_atomic_t *quit,
                     const mf_plugin_registry_t *reg)
{
    memset(g_dev, 0, sizeof(g_dev));
    g_pts = pts;
    g_chan_tick = chan_tick;
    g_quit = quit;
    g_reg = reg;
}

const mf_plugin_registry_t *mf_devices_registry(void)
{
    return g_reg;
}

int mf_devices_visit_live(int (*fn)(const mf_devinfo_t *, void *), void *arg)
{
    int i, n = 0;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        mf_devinfo_t info;
        int rc;
        if (!g_dev[i].in_use || g_dev[i].stop)
            continue;
        fill_info(&g_dev[i], &info);
        rc = fn(&info, arg);
        n++;
        if (rc != 0)
            return rc;
    }
    return n;
}

int mf_devices_find_live(const char *uuid, mf_devinfo_t *out)
{
    int i;
    if (!uuid || !out)
        return -1;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (!g_dev[i].in_use || g_dev[i].stop)
            continue;
        if (strcmp(g_dev[i].uuid, uuid) == 0) {
            fill_info(&g_dev[i], out);
            return 0;
        }
    }
    return -1;
}

int mf_devices_add(const char *name, const char *kind, const char *driver,
                   const char *spec_json, char *uuid_out, size_t uuid_cap,
                   char *err, size_t errsz)
{
    int i, slot = -1;
    mf_device_t *d;
    const mf_plugin_ops_t *ops = NULL;
    char uuid[MF_UUID_LEN];
    char openerr[96];

    if (err && errsz)
        err[0] = '\0';
    if (!name || !name[0] || !kind || !kind[0] || !driver || !driver[0]) {
        if (err && errsz)
            snprintf(err, errsz, "name, kind, and driver are required");
        return -1;
    }
    if (strlen(name) >= sizeof(g_dev[0].name)) {
        if (err && errsz)
            snprintf(err, errsz, "name too long");
        return -1;
    }
    if (name_taken(name, -1)) {
        if (err && errsz)
            snprintf(err, errsz, "name in use");
        return -3;
    }
    if (g_reg && g_reg->nops > 0) {
        ops = mf_plugins_find(g_reg, kind, driver);
        if (!ops) {
            if (err && errsz)
                snprintf(err, errsz, "unknown kind/driver");
            return -2;
        }
    }
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (!g_dev[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (err && errsz)
            snprintf(err, errsz, "device limit reached");
        return -4;
    }

    uuid_generate(uuid, sizeof(uuid));
    d = &g_dev[slot];
    memset(d, 0, sizeof(*d));
    snprintf(d->uuid, sizeof(d->uuid), "%s", uuid);
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->kind, sizeof(d->kind), "%s", kind);
    snprintf(d->driver, sizeof(d->driver), "%s", driver);
    d->ops = ops;
    d->poll_interval_s = 2.0;
    snprintf(d->reading_json, sizeof(d->reading_json), "{}");

    if (ops && ops->open) {
        openerr[0] = '\0';
        d->ctx = ops->open(spec_json ? spec_json : "{}", openerr, sizeof(openerr));
        if (!d->ctx) {
            if (err && errsz)
                snprintf(err, errsz, "%s", openerr[0] ? openerr : "open failed");
            memset(d, 0, sizeof(*d));
            return -5;
        }
        if (ops->caps)
            d->caps = ops->caps(d->ctx);
        refresh_reading(d);
    }

    d->in_use = true;
    d->stop = false;
    d->env.idx = slot;
    /* Do not protothread_run() here: POST is called from http_conn_pt. */
    if (g_pts && g_chan_tick)
        pt_create(g_pts, &d->thr, device_pt, &d->env);
    if (uuid_out && uuid_cap)
        snprintf(uuid_out, uuid_cap, "%s", uuid);
    LOG_I("device add %s kind=%s driver=%s uuid=%s", name, kind, driver, uuid);
    return 0;
}

double mf_poll_interval_min(const char *driver)
{
    if (driver && strcmp(driver, "classic") == 0)
        return 1.0;
    if (driver && strcmp(driver, "jk") == 0)
        return 1.0;
    if (driver && strcmp(driver, "xd") == 0)
        return 0.5;
    return 0.5;
}

static mf_device_t *find_live_mut(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return NULL;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (!g_dev[i].in_use || g_dev[i].stop)
            continue;
        if (strcmp(g_dev[i].uuid, uuid) == 0)
            return &g_dev[i];
    }
    return NULL;
}

int mf_devices_get_settings(const char *uuid, char *json, size_t cap)
{
    mf_device_t *d = find_live_mut(uuid);
    char plug[4096];

    if (!d || !json || cap == 0)
        return 404;
    plug[0] = '\0';
    if (d->ops && d->ops->get_settings && d->ctx)
        (void)d->ops->get_settings(d->ctx, plug, sizeof(plug));
    if (plug[0] == '{' && plug[1] && plug[1] != '}')
        snprintf(json, cap, "{\"poll_interval_s\":%.3f,%s",
                 d->poll_interval_s, plug + 1);
    else
        snprintf(json, cap, "{\"poll_interval_s\":%.3f}", d->poll_interval_s);
    json[cap - 1] = '\0';
    return 200;
}

int mf_devices_put_settings(const char *uuid, const char *json,
                            char *err, size_t errsz)
{
    mf_device_t *d = find_live_mut(uuid);
    const char *p;
    int have_poll = 0;
    int have_other = 0;
    int rc;

    if (err && errsz)
        err[0] = '\0';
    if (!d)
        return 404;
    if (!json)
        json = "{}";
    p = strstr(json, "\"poll_interval_s\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            double iv = atof(p + 1);
            double mn = mf_poll_interval_min(d->driver);
            have_poll = 1;
            if (iv < mn) {
                if (err && errsz)
                    snprintf(err, errsz, "poll_interval_s below %.1f", mn);
                return 400;
            }
            d->poll_interval_s = iv;
        }
    }
    {
        const char *q = json;
        have_other = 0;
        while ((q = strchr(q, '"')) != NULL) {
            const char *k = q + 1;
            const char *e = strchr(k, '"');
            if (!e)
                break;
            if ((size_t)(e - k) != 15 || strncmp(k, "poll_interval_s", 15) != 0)
                have_other = 1;
            q = e + 1;
        }
    }
    if (d->ops && d->ops->put_settings && d->ctx && have_other) {
        rc = d->ops->put_settings(d->ctx, json, err, errsz);
        if (rc == MF_ERR_UNSUPPORTED || rc == MF_ERR_INVAL)
            return 400;
        if (rc == MF_ERR_OFFLINE || rc == MF_ERR_BUSY)
            return 409;
        if (rc != MF_OK && rc != 0)
            return 400;
    }
    (void)have_poll;
    return 200;
}

int mf_devices_action(const char *uuid, const char *action, const char *json,
                      char *err, size_t errsz)
{
    mf_device_t *d = find_live_mut(uuid);
    int rc;

    if (err && errsz)
        err[0] = '\0';
    if (!d)
        return 404;
    if (!action || !action[0])
        return 400;
    if (!d->ops || !d->ops->action || !d->ctx) {
        if (err && errsz)
            snprintf(err, errsz, "unsupported");
        return 400;
    }
    rc = d->ops->action(d->ctx, action, json ? json : "{}", err, errsz);
    if (rc == MF_OK || rc == 0) {
        refresh_reading(d);
        return 200;
    }
    if (rc == MF_ERR_OFFLINE || rc == MF_ERR_BUSY)
        return 409;
    return 400;
}

int mf_devices_any_dying(void)
{
    int i;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_dev[i].in_use && g_dev[i].stop)
            return 1;
    }
    return 0;
}

int mf_devices_delete(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return 404;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        mf_device_t *d = &g_dev[i];
        if (!d->in_use)
            continue;
        if (strcmp(d->uuid, uuid) != 0)
            continue;
        d->stop = true;
        if (!g_pts) {
            /* Route-unit tests: no protothread, free immediately. */
            slot_clear(d);
        }
        LOG_I("device delete %s (stop)", uuid);
        return 202;
    }
    return 404;
}

void mf_devices_prepare_fds(fd_set *rset, fd_set *wset, int *maxfd)
{
    int i;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        mf_device_t *d = &g_dev[i];
        int fd;
        unsigned mask;
        if (!d->in_use || d->stop || !d->ops || !d->ctx)
            continue;
        if (d->ops->prepare_fds) {
            d->ops->prepare_fds(d->ctx, rset, wset, maxfd);
            continue;
        }
        if (!d->ops->fd || !d->ops->select_mask)
            continue;
        fd = d->ops->fd(d->ctx);
        mask = d->ops->select_mask(d->ctx);
        if (fd < 0)
            continue;
        if ((mask & MF_IO_WANT_READ) && rset)
            FD_SET(fd, rset);
        if ((mask & MF_IO_WANT_WRITE) && wset)
            FD_SET(fd, wset);
        if (maxfd && fd > *maxfd)
            *maxfd = fd;
    }
}

void mf_devices_request_stop_all(void)
{
    int i;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_dev[i].in_use)
            g_dev[i].stop = true;
    }
}
