/*
 * Device slots: live / dying / free. No pt_kill. LIST/GET walk live only.
 * Uniqueness of name among all in_use (including dying).
 */

#include "device.h"

#include <cJSON.h>
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
    char     bus[32];
    char     endpoint[160];
    bool     enabled;
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

typedef struct {
    bool               used;
    mf_config_device_t dev;
} mf_pending_add_t;

static mf_pending_add_t g_pending[MF_DEVICE_SLOTS];

static pt_t device_pt(env_t e_);
double mf_poll_interval_min(const char *driver);

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
    out->endpoint = d->endpoint;
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
    memset(g_pending, 0, sizeof(g_pending));
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

static void identity_from_spec(const char *spec_json, char *bus, size_t buscap,
                               char *ep, size_t epcap, double *poll_s)
{
    cJSON *root, *it, *usb, *ble, *mb;
    if (bus && buscap)
        bus[0] = '\0';
    if (ep && epcap)
        ep[0] = '\0';
    if (!spec_json || !spec_json[0])
        return;
    root = cJSON_Parse(spec_json);
    if (!root)
        return;
    it = cJSON_GetObjectItemCaseSensitive(root, "bus");
    if (bus && buscap && cJSON_IsString(it) && it->valuestring)
        snprintf(bus, buscap, "%s", it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(root, "poll_interval_s");
    if (poll_s && cJSON_IsNumber(it) && it->valuedouble > 0.0)
        *poll_s = it->valuedouble;
    usb = cJSON_GetObjectItemCaseSensitive(root, "usb");
    ble = cJSON_GetObjectItemCaseSensitive(root, "ble");
    mb = cJSON_GetObjectItemCaseSensitive(root, "modbus");
    if (ep && epcap) {
        if (cJSON_IsObject(usb)) {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(usb, "path");
            cJSON *s = cJSON_GetObjectItemCaseSensitive(usb, "serial_id");
            if (cJSON_IsString(p) && p->valuestring && p->valuestring[0])
                snprintf(ep, epcap, "usb:%s", p->valuestring);
            else if (cJSON_IsString(s) && s->valuestring && s->valuestring[0])
                snprintf(ep, epcap, "usb-id:%s", s->valuestring);
        }
        if (!ep[0] && cJSON_IsObject(ble)) {
            cJSON *a = cJSON_GetObjectItemCaseSensitive(ble, "address");
            if (cJSON_IsString(a) && a->valuestring && a->valuestring[0])
                snprintf(ep, epcap, "ble:%s", a->valuestring);
        }
        if (!ep[0] && cJSON_IsObject(mb)) {
            cJSON *ip = cJSON_GetObjectItemCaseSensitive(mb, "ip");
            cJSON *port = cJSON_GetObjectItemCaseSensitive(mb, "port");
            int pn = 502;
            if (cJSON_IsNumber(port) && port->valueint > 0)
                pn = port->valueint;
            if (cJSON_IsString(ip) && ip->valuestring && ip->valuestring[0])
                snprintf(ep, epcap, "tcp:%s:%d", ip->valuestring, pn);
        }
    }
    cJSON_Delete(root);
}

static int uuid_in_use(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return 0;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_dev[i].in_use && strcmp(g_dev[i].uuid, uuid) == 0)
            return 1;
    }
    return 0;
}

int mf_devices_add(const char *name, const char *kind, const char *driver,
                   const char *spec_json, const char *uuid_in,
                   char *uuid_out, size_t uuid_cap,
                   char *err, size_t errsz)
{
    int i, slot = -1;
    mf_device_t *d;
    const mf_plugin_ops_t *ops = NULL;
    char uuid[MF_UUID_LEN];
    char openerr[96];
    double poll = 2.0;

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
    if (uuid_in && uuid_in[0] && uuid_in_use(uuid_in)) {
        if (err && errsz)
            snprintf(err, errsz, "uuid in use");
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

    if (uuid_in && uuid_in[0])
        snprintf(uuid, sizeof(uuid), "%s", uuid_in);
    else
        uuid_generate(uuid, sizeof(uuid));
    d = &g_dev[slot];
    memset(d, 0, sizeof(*d));
    snprintf(d->uuid, sizeof(d->uuid), "%s", uuid);
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->kind, sizeof(d->kind), "%s", kind);
    snprintf(d->driver, sizeof(d->driver), "%s", driver);
    d->ops = ops;
    d->enabled = true;
    identity_from_spec(spec_json, d->bus, sizeof(d->bus),
                       d->endpoint, sizeof(d->endpoint), &poll);
    {
        double mn = mf_poll_interval_min(d->driver);
        d->poll_interval_s = poll < mn ? mn : poll;
    }
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
    cJSON *plug_root = NULL, *out, *it;
    char *printed;

    if (!d || !json || cap == 0)
        return 404;
    plug[0] = '\0';
    if (d->ops && d->ops->get_settings && d->ctx)
        (void)d->ops->get_settings(d->ctx, plug, sizeof(plug));

    out = cJSON_CreateObject();
    if (!out)
        return 500;
    cJSON_AddStringToObject(out, "name", d->name);
    cJSON_AddNumberToObject(out, "poll_interval_s", d->poll_interval_s);
    if (plug[0] == '{')
        plug_root = cJSON_Parse(plug);
    if (plug_root && cJSON_IsObject(plug_root)) {
        for (it = plug_root->child; it; it = it->next) {
            cJSON *copy;

            if (!it->string || !it->string[0])
                continue;
            if (strcmp(it->string, "poll_interval_s") == 0 ||
                strcmp(it->string, "name") == 0 ||
                strcmp(it->string, "uuid") == 0)
                continue;
            copy = cJSON_Duplicate(it, 1);
            if (copy)
                cJSON_AddItemToObject(out, it->string, copy);
        }
    }
    if (plug_root)
        cJSON_Delete(plug_root);
    cJSON_AddStringToObject(out, "uuid", d->uuid);

    printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!printed) {
        snprintf(json, cap, "{\"poll_interval_s\":%.3f}", d->poll_interval_s);
        json[cap - 1] = '\0';
        return 200;
    }
    snprintf(json, cap, "%s", printed);
    json[cap - 1] = '\0';
    free(printed);
    return 200;
}

int mf_devices_put_settings(const char *uuid, const char *json,
                            char *err, size_t errsz)
{
    mf_device_t *d = find_live_mut(uuid);
    cJSON *root, *it;
    int have_other = 0;
    int rc;
    int slot;

    if (err && errsz)
        err[0] = '\0';
    if (!d)
        return 404;
    if (!json)
        json = "{}";
    slot = (int)(d - g_dev);
    root = cJSON_Parse(json);
    if (root && cJSON_IsObject(root)) {
        it = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(it) && it->valuestring) {
            if (!it->valuestring[0]) {
                if (err && errsz)
                    snprintf(err, errsz, "name required");
                cJSON_Delete(root);
                return 400;
            }
            if (strlen(it->valuestring) >= sizeof(d->name)) {
                if (err && errsz)
                    snprintf(err, errsz, "name too long");
                cJSON_Delete(root);
                return 400;
            }
            if (name_taken(it->valuestring, slot)) {
                if (err && errsz)
                    snprintf(err, errsz, "name in use");
                cJSON_Delete(root);
                return 400;
            }
            snprintf(d->name, sizeof(d->name), "%s", it->valuestring);
        }
        it = cJSON_GetObjectItemCaseSensitive(root, "poll_interval_s");
        if (cJSON_IsNumber(it) ||
            (cJSON_IsString(it) && it->valuestring)) {
            double iv = cJSON_IsNumber(it) ? it->valuedouble
                                           : atof(it->valuestring);
            double mn = mf_poll_interval_min(d->driver);

            if (iv < mn) {
                if (err && errsz)
                    snprintf(err, errsz, "poll_interval_s below %.1f", mn);
                cJSON_Delete(root);
                return 400;
            }
            d->poll_interval_s = iv;
        }
        for (it = root->child; it; it = it->next) {
            if (!it->string || !it->string[0])
                continue;
            if (strcmp(it->string, "name") == 0 ||
                strcmp(it->string, "poll_interval_s") == 0 ||
                strcmp(it->string, "uuid") == 0)
                continue;
            have_other = 1;
        }
    }
    if (root)
        cJSON_Delete(root);
    if (d->ops && d->ops->put_settings && d->ctx && have_other) {
        rc = d->ops->put_settings(d->ctx, json, err, errsz);
        if (rc == MF_ERR_UNSUPPORTED || rc == MF_ERR_INVAL)
            return 400;
        if (rc == MF_ERR_OFFLINE || rc == MF_ERR_BUSY)
            return 409;
        if (rc != MF_OK && rc != 0)
            return 400;
    }
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

static void pending_cancel(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_pending[i].used && strcmp(g_pending[i].dev.uuid, uuid) == 0)
            g_pending[i].used = false;
    }
}

static void pending_queue(const mf_config_device_t *dev)
{
    int i, slot = -1;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_pending[i].used &&
            strcmp(g_pending[i].dev.uuid, dev->uuid) == 0) {
            g_pending[i].dev = *dev;
            return;
        }
        if (!g_pending[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return;
    g_pending[slot].used = true;
    g_pending[slot].dev = *dev;
}

static int find_slot_uuid(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return -1;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_dev[i].in_use && strcmp(g_dev[i].uuid, uuid) == 0)
            return i;
    }
    return -1;
}

static int uuid_in_cfg(const mf_config_device_t *devs, int n, const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return 0;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].uuid, uuid) == 0)
            return 1;
    }
    return 0;
}

static void stop_slot(mf_device_t *d)
{
    if (!d->in_use)
        return;
    d->stop = true;
    if (!g_pts)
        slot_clear(d);
}

static int add_from_cfg(const mf_config_device_t *dev, char *err, size_t errsz)
{
    char *spec = mf_config_device_serialize(dev);
    int rc = mf_devices_add(dev->name, dev->kind, dev->driver,
                            spec ? spec : "{}", dev->uuid, NULL, 0,
                            err, errsz);
    free(spec);
    return rc;
}

static int identity_changed(const mf_device_t *d, const mf_config_device_t *n)
{
    char ep[160];
    mf_config_device_endpoint(n, ep, sizeof(ep));
    if (strcmp(d->kind, n->kind) != 0)
        return 1;
    if (strcmp(d->driver, n->driver) != 0)
        return 1;
    if (strcmp(d->bus, n->bus) != 0)
        return 1;
    if (strcmp(d->endpoint, ep) != 0)
        return 1;
    return 0;
}

static int validate_apply(const mf_config_device_t *devs, int n,
                          char *err, size_t errsz)
{
    int i, j, s;

    for (i = 0; i < n; i++) {
        if (!devs[i].uuid[0]) {
            if (err && errsz)
                snprintf(err, errsz, "device missing uuid");
            return -1;
        }
        if (devs[i].enabled &&
            (!devs[i].name[0] || !devs[i].kind[0] || !devs[i].driver[0])) {
            if (err && errsz)
                snprintf(err, errsz, "name, kind, and driver are required");
            return -1;
        }
        if (devs[i].name[0] && strlen(devs[i].name) >= sizeof(g_dev[0].name)) {
            if (err && errsz)
                snprintf(err, errsz, "name too long");
            return -1;
        }
        for (j = i + 1; j < n; j++) {
            if (strcmp(devs[i].uuid, devs[j].uuid) == 0) {
                if (err && errsz)
                    snprintf(err, errsz, "duplicate uuid");
                return -1;
            }
            if (devs[i].name[0] && strcmp(devs[i].name, devs[j].name) == 0) {
                if (err && errsz)
                    snprintf(err, errsz, "duplicate name");
                return -1;
            }
            {
                char a[160], b[160];
                mf_config_device_endpoint(&devs[i], a, sizeof(a));
                mf_config_device_endpoint(&devs[j], b, sizeof(b));
                if (a[0] && b[0] && strcmp(a, b) == 0) {
                    if (err && errsz)
                        snprintf(err, errsz, "duplicate endpoint");
                    return -1;
                }
            }
        }
        if (devs[i].enabled && g_reg && g_reg->nops > 0) {
            if (!mf_plugins_find(g_reg, devs[i].kind, devs[i].driver)) {
                if (err && errsz)
                    snprintf(err, errsz, "unknown kind/driver");
                return -1;
            }
        }
        if (devs[i].poll_interval_s > 0.0) {
            double mn = mf_poll_interval_min(devs[i].driver);
            if (devs[i].poll_interval_s < mn) {
                if (err && errsz)
                    snprintf(err, errsz, "poll_interval_s below %.1f", mn);
                return -1;
            }
        }
    }

    for (i = 0; i < n; i++) {
        char ep[160];
        if (!devs[i].enabled)
            continue;
        mf_config_device_endpoint(&devs[i], ep, sizeof(ep));
        for (s = 0; s < MF_DEVICE_SLOTS; s++) {
            mf_device_t *d = &g_dev[s];
            int name_hit, ep_hit;
            if (!d->in_use)
                continue;
            if (strcmp(d->uuid, devs[i].uuid) == 0)
                continue;
            name_hit = devs[i].name[0] && strcmp(d->name, devs[i].name) == 0;
            ep_hit = ep[0] && d->endpoint[0] && strcmp(d->endpoint, ep) == 0;
            if (!name_hit && !ep_hit)
                continue;
            if (uuid_in_cfg(devs, n, d->uuid))
                continue;
            if (err && errsz)
                snprintf(err, errsz, "channel still stopping");
            return -1;
        }
    }
    return 0;
}

int mf_devices_validate_config(const mf_config_device_t *devs, int n,
                               char *err, size_t errsz)
{
    if (err && errsz)
        err[0] = '\0';
    if (n < 0)
        n = 0;
    if (n > 0 && !devs) {
        if (err && errsz)
            snprintf(err, errsz, "bad request");
        return -1;
    }
    return validate_apply(devs, n, err, errsz);
}

int mf_devices_apply_config(const mf_config_device_t *devs, int n,
                            char *err, size_t errsz)
{
    int i, s, pending = 0;

    if (mf_devices_validate_config(devs, n, err, errsz) != 0)
        return -1;

    /* Patch identity-same; stop REMOVE and identity changes. */
    for (s = 0; s < MF_DEVICE_SLOTS; s++) {
        mf_device_t *d = &g_dev[s];
        const mf_config_device_t *want = NULL;
        if (!d->in_use)
            continue;
        for (i = 0; i < n; i++) {
            if (strcmp(devs[i].uuid, d->uuid) == 0) {
                want = &devs[i];
                break;
            }
        }
        if (!want || !want->enabled) {
            pending_cancel(d->uuid);
            stop_slot(d);
            continue;
        }
        if (identity_changed(d, want)) {
            pending_queue(want);
            stop_slot(d);
            continue;
        }
        snprintf(d->name, sizeof(d->name), "%.*s",
                 (int)sizeof(d->name) - 1, want->name);
        d->enabled = want->enabled;
        if (want->poll_interval_s > 0.0) {
            double mn = mf_poll_interval_min(d->driver);
            d->poll_interval_s = want->poll_interval_s < mn ? mn
                                                            : want->poll_interval_s;
        }
    }

    /* ADD new enabled UUIDs. */
    for (i = 0; i < n; i++) {
        char openerr[96];
        int rc;
        if (!devs[i].enabled)
            continue;
        if (find_slot_uuid(devs[i].uuid) >= 0)
            continue;
        if (uuid_in_use(devs[i].uuid)) {
            pending_queue(&devs[i]);
            continue;
        }
        openerr[0] = '\0';
        rc = add_from_cfg(&devs[i], openerr, sizeof(openerr));
        if (rc == -3) {
            /* Name still held by a dying other uuid — validate should have
             * caught this; queue if same uuid is dying. */
            pending_queue(&devs[i]);
        } else if (rc != 0) {
            LOG_W("config apply open %s: %s",
                  devs[i].name[0] ? devs[i].name : devs[i].uuid,
                  openerr[0] ? openerr : "failed");
        }
    }

    if (mf_devices_any_dying())
        pending = 1;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        if (g_pending[i].used)
            pending = 1;
    }
    return pending ? 1 : 0;
}

void mf_devices_apply_pending(void)
{
    int i;
    for (i = 0; i < MF_DEVICE_SLOTS; i++) {
        char openerr[96];
        int rc;
        if (!g_pending[i].used)
            continue;
        if (uuid_in_use(g_pending[i].dev.uuid))
            continue;
        openerr[0] = '\0';
        rc = add_from_cfg(&g_pending[i].dev, openerr, sizeof(openerr));
        if (rc == 0) {
            g_pending[i].used = false;
            continue;
        }
        if (rc == -3)
            continue; /* still colliding; retry next tick */
        LOG_W("pending open %s: %s",
              g_pending[i].dev.name[0] ? g_pending[i].dev.name
                                       : g_pending[i].dev.uuid,
              openerr[0] ? openerr : "failed");
        g_pending[i].used = false;
    }
}

int mf_devices_delete(const char *uuid)
{
    int i;
    if (!uuid || !uuid[0])
        return 404;
    pending_cancel(uuid);
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
