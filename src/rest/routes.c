#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "config/config.h"
#include "rest.h"
#include "device.h"
#include "discover.h"
#include "history.h"
#include "system/system.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

void mf_log(int prio, const char *fmt, ...);

static mf_daemon_config_t  g_cfg_local;
static mf_daemon_config_t *g_live;
static char                g_cfg_path[256];

void mf_rest_set_live_config(void *daemon_cfg, const char *path)
{
    g_live = daemon_cfg;
    g_cfg_path[0] = '\0';
    if (path && path[0])
        snprintf(g_cfg_path, sizeof(g_cfg_path), "%s", path);
}

const char *mf_rest_listen_spec(void)
{
    if (g_live && g_live->listen[0])
        return g_live->listen;
    return "0.0.0.0:5250";
}

static mf_daemon_config_t *live_cfg(void)
{
    if (!g_live)
    {
        mf_config_defaults(&g_cfg_local);
        g_live = &g_cfg_local;
    }
    return g_live;
}

static void cfg_remove_uuid(const char *uuid)
{
    mf_daemon_config_t *cfg = live_cfg();
    int i;
    if (!uuid || !uuid[0])
        return;
    for (i = 0; i < cfg->n_devices; i++)
    {
        if (strcmp(cfg->devices[i].uuid, uuid) != 0)
            continue;
        if (i + 1 < cfg->n_devices)
            memmove(&cfg->devices[i], &cfg->devices[i + 1],
                    (size_t)(cfg->n_devices - i - 1) * sizeof(cfg->devices[0]));
        cfg->n_devices--;
        memset(&cfg->devices[cfg->n_devices], 0, sizeof(cfg->devices[0]));
        return;
    }
}

/* Mirror the daemon-owned settings from an accepted PUT into the config
 * so they are saved and survive a restart. */
static void cfg_patch_settings(const char *uuid, const cJSON *body)
{
    mf_daemon_config_t *cfg = live_cfg();
    mf_config_device_t *d = NULL;
    const cJSON *it;
    int i;

    if (!uuid || !uuid[0] || !cfg || !body)
        return;
    for (i = 0; i < cfg->n_devices; i++)
    {
        if (strcmp(cfg->devices[i].uuid, uuid) == 0)
        {
            d = &cfg->devices[i];
            break;
        }
    }
    if (!d)
        return;
    it = cJSON_GetObjectItemCaseSensitive(body, "name");
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0])
        snprintf(d->name, sizeof(d->name), "%s", it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(body, "poll_interval_s");
    if (cJSON_IsNumber(it) && it->valuedouble > 0.0)
        d->poll_interval_s = it->valuedouble;
    else if (cJSON_IsString(it) && it->valuestring && atof(it->valuestring) > 0.0)
        d->poll_interval_s = atof(it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(body, "capture_interval_s");
    if (cJSON_IsNumber(it))
        d->capture_interval_s = it->valuedouble;
    else if (cJSON_IsString(it) && it->valuestring)
        d->capture_interval_s = atof(it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(body, "active");
    if (cJSON_IsBool(it))
        d->active = cJSON_IsTrue(it);
}

static void cfg_upsert_from_json(const char *uuid, const cJSON *root)
{
    mf_daemon_config_t *cfg = live_cfg();
    mf_config_device_t *d = NULL;
    cJSON *copy;
    int i;
    if (!uuid || !uuid[0] || !root)
        return;
    for (i = 0; i < cfg->n_devices; i++)
    {
        if (strcmp(cfg->devices[i].uuid, uuid) == 0)
        {
            d = &cfg->devices[i];
            break;
        }
    }
    if (!d)
    {
        if (cfg->n_devices >= MF_MAX_DEVICES)
            return;
        d = &cfg->devices[cfg->n_devices++];
        memset(d, 0, sizeof(*d));
    }
    copy = cJSON_Duplicate(root, 1);
    if (!copy)
        return;
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "uuid");
    cJSON_AddStringToObject(copy, "uuid", uuid);
    (void)mf_config_device_from_json(d, copy);
    cJSON_Delete(copy);
    snprintf(d->uuid, sizeof(d->uuid), "%s", uuid);
    d->enabled = true;
}

static void set_error(mf_rest_response_t *resp, int status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    char *s;

    cJSON_AddStringToObject(root, "error", msg ? msg : "error");
    s = cJSON_PrintUnformatted(root);
    resp->status = status;
    resp->location[0] = '\0';
    resp->etag[0] = '\0';
    resp->body[0] = '\0';
    resp->body_len = 0;
    if (s)
    {
        size_t n = strlen(s);
        if (n >= sizeof(resp->body))
            n = sizeof(resp->body) - 1;
        memcpy(resp->body, s, n);
        resp->body[n] = '\0';
        resp->body_len = n;
        free(s);
    }
    cJSON_Delete(root);
}

static void set_json(mf_rest_response_t *resp, int status, cJSON *root,
                     const char *location)
{
    char *s = cJSON_PrintUnformatted(root);

    resp->status = status;
    resp->etag[0] = '\0';
    resp->location[0] = '\0';
    resp->body[0] = '\0';
    resp->body_len = 0;
    if (s)
    {
        size_t n = strlen(s);
        if (n >= sizeof(resp->body))
            n = sizeof(resp->body) - 1;
        memcpy(resp->body, s, n);
        resp->body[n] = '\0';
        resp->body_len = n;
        free(s);
    }
    if (location && location[0])
    {
        size_t n = strlen(location);
        if (n >= sizeof(resp->location))
            n = sizeof(resp->location) - 1;
        memcpy(resp->location, location, n);
        resp->location[n] = '\0';
    }
    cJSON_Delete(root);
}

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it) || !it->valuestring)
        return NULL;
    return it->valuestring;
}

static cJSON *num_or_null(cJSON *obj, const char *key)
{
    cJSON *it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return (it && cJSON_IsNumber(it)) ? it : NULL;
}

static cJSON *str_or_null(cJSON *obj, const char *key)
{
    cJSON *it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return (it && cJSON_IsString(it)) ? it : NULL;
}

typedef struct {
    cJSON              *root;
    mf_system_totals_t  sys;
} status_ctx_t;

static double num_or(const cJSON *n, double dflt)
{
    return n ? n->valuedouble : dflt;
}

static int add_status_row(const mf_devinfo_t *d, void *arg)
{
    status_ctx_t *ctx = arg;
    cJSON *root = ctx->root;
    cJSON *batteries = cJSON_GetObjectItemCaseSensitive(root, "batteries");
    cJSON *chargers = cJSON_GetObjectItemCaseSensitive(root, "chargers");
    cJSON *inverters = cJSON_GetObjectItemCaseSensitive(root, "inverters");
    cJSON *phantoms = cJSON_GetObjectItemCaseSensitive(root, "phantoms");
    cJSON *row = cJSON_CreateObject();
    cJSON *data = cJSON_Parse(d->reading_json);

    cJSON_AddStringToObject(row, "id", d->uuid);
    cJSON_AddStringToObject(row, "name", d->name);
    cJSON_AddStringToObject(row, "driver", d->driver);
    cJSON_AddBoolToObject(row, "online", d->online);
    cJSON_AddBoolToObject(row, "active", d->active);
    cJSON_AddNumberToObject(row, "seq", (double)d->seq);

    if (strcmp(d->kind, "charger") == 0)
    {
        cJSON *v = num_or_null(data, "battery_voltage_v");
        cJSON *w = num_or_null(data, "charging_watts");
        cJSON *k = num_or_null(data, "kwh_today");
        cJSON *st = str_or_null(data, "charge_stage");
        if (v)
            cJSON_AddNumberToObject(row, "battery_voltage_v", v->valuedouble);
        if (w)
            cJSON_AddNumberToObject(row, "charging_watts", w->valuedouble);
        if (k)
            cJSON_AddNumberToObject(row, "kwh_today", k->valuedouble);
        if (st)
            cJSON_AddStringToObject(row, "charge_stage", st->valuestring);
        mf_system_add_charger(&ctx->sys, d->active, d->online, num_or(w, 0.0));
        cJSON_AddItemToArray(chargers, row);
    } else if (strcmp(d->kind, "inverter") == 0)
    {
        cJSON_AddItemToArray(inverters, row);
    } else if (strcmp(d->kind, "phantom") == 0)
    {
        cJSON_AddItemToArray(phantoms, row);
    }
    else
    {
        cJSON *v = num_or_null(data, "pack_voltage_v");
        cJSON *c = num_or_null(data, "current_a");
        cJSON *s = num_or_null(data, "soc_pct");
        cJSON *n = num_or_null(data, "cell_count");
        cJSON *full = num_or_null(data, "full_capacity_ah");
        cJSON *rem = num_or_null(data, "remaining_capacity_ah");
        if (v)
            cJSON_AddNumberToObject(row, "pack_voltage_v", v->valuedouble);
        if (c)
            cJSON_AddNumberToObject(row, "current_a", c->valuedouble);
        if (s)
            cJSON_AddNumberToObject(row, "soc_pct", s->valuedouble);
        if (n)
            cJSON_AddNumberToObject(row, "cell_count", n->valuedouble);
        if (full)
            cJSON_AddNumberToObject(row, "full_capacity_ah", full->valuedouble);
        if (rem)
            cJSON_AddNumberToObject(row, "remaining_capacity_ah",
                                    rem->valuedouble);
        mf_system_add_battery(&ctx->sys, d->active, d->online,
                              num_or(v, 0.0), num_or(c, 0.0), num_or(s, 0.0),
                              num_or(rem, -1.0), num_or(full, 0.0),
                              (int)num_or(n, 0.0));
        cJSON_AddItemToArray(batteries, row);
    }
    if (data)
        cJSON_Delete(data);
    return 0;
}

static void add_caps(cJSON *arr, unsigned caps)
{
    if (caps & MF_CAP_READ)
        cJSON_AddItemToArray(arr, cJSON_CreateString("read"));
    if (caps & MF_CAP_WRITE_SETTINGS)
        cJSON_AddItemToArray(arr, cJSON_CreateString("write_settings"));
    if (caps & MF_CAP_ACTION_SWITCH)
        cJSON_AddItemToArray(arr, cJSON_CreateString("action.switch"));
    if (caps & MF_CAP_ACTION_REFRESH)
        cJSON_AddItemToArray(arr, cJSON_CreateString("action.refresh"));
    if (caps & MF_CAP_PROBE)
        cJSON_AddItemToArray(arr, cJSON_CreateString("probe"));
    if (caps & MF_CAP_AUTO_PORT)
        cJSON_AddItemToArray(arr, cJSON_CreateString("auto_port"));
    if (caps & MF_CAP_AUTO_NET)
        cJSON_AddItemToArray(arr, cJSON_CreateString("auto_net"));
}

static cJSON *system_json(const mf_system_totals_t *t)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "input_w", t->input_w);
    cJSON_AddNumberToObject(o, "input_max_w", t->input_max_w);
    if (t->soc_pct >= 0.0)
        cJSON_AddNumberToObject(o, "soc_pct", t->soc_pct);
    else
        cJSON_AddNullToObject(o, "soc_pct");
    cJSON_AddNumberToObject(o, "stored_wh", t->stored_wh);
    cJSON_AddNumberToObject(o, "capacity_wh", t->capacity_wh);
    cJSON_AddNumberToObject(o, "charge_w", t->charge_w);
    cJSON_AddNumberToObject(o, "discharge_w", t->discharge_w);
    cJSON_AddNumberToObject(o, "discharge_max_w", t->discharge_max_w);
    cJSON_AddNumberToObject(o, "chargers_counted", t->chargers_counted);
    cJSON_AddNumberToObject(o, "chargers_total", t->chargers_total);
    cJSON_AddNumberToObject(o, "batteries_counted", t->batteries_counted);
    cJSON_AddNumberToObject(o, "batteries_total", t->batteries_total);
    return o;
}

static int handle_status(mf_rest_response_t *resp)
{
    status_ctx_t ctx;
    const mf_daemon_config_t *cfg = live_cfg();

    ctx.root = cJSON_CreateObject();
    mf_system_init(&ctx.sys, cfg->system.input_max_w,
                   cfg->system.discharge_max_w);
    cJSON_AddStringToObject(ctx.root, "server", "moonflared/" MF_VERSION);
    cJSON_AddNumberToObject(ctx.root, "ts", (double)time(NULL));
    cJSON_AddItemToObject(ctx.root, "batteries", cJSON_CreateArray());
    cJSON_AddItemToObject(ctx.root, "chargers", cJSON_CreateArray());
    cJSON_AddItemToObject(ctx.root, "inverters", cJSON_CreateArray());
    cJSON_AddItemToObject(ctx.root, "phantoms", cJSON_CreateArray());
    mf_devices_visit_live(add_status_row, &ctx);
    mf_system_finish(&ctx.sys);
    cJSON_AddItemToObject(ctx.root, "system", system_json(&ctx.sys));
    set_json(resp, 200, ctx.root, NULL);
    return 0;
}

static int handle_drivers(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    const mf_plugin_registry_t *reg = mf_devices_registry();
    int i;

    if (reg && reg->nops > 0)
    {
        for (i = 0; i < reg->nops; i++)
        {
            cJSON *o = cJSON_CreateObject();
            const mf_plugin_ops_t *ops = reg->ops[i];
            cJSON_AddStringToObject(o, "kind", ops->kind ? ops->kind : "");
            cJSON_AddStringToObject(o, "driver", ops->driver ? ops->driver : "");
            {
                cJSON *caps = cJSON_CreateArray();
                unsigned c = 0;
                if (ops->caps)
                    c = ops->caps(NULL);
                add_caps(caps, c);
                cJSON_AddItemToObject(o, "caps", caps);
            }
            cJSON_AddItemToArray(arr, o);
        }
    }
    else
    {
        cJSON *b = cJSON_CreateObject();
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "kind", "battery");
        cJSON_AddStringToObject(b, "driver", "demo");
        cJSON_AddItemToArray(arr, b);
        cJSON_AddStringToObject(c, "kind", "charger");
        cJSON_AddStringToObject(c, "driver", "demo");
        cJSON_AddItemToArray(arr, c);
    }
    cJSON_AddItemToObject(root, "drivers", arr);
    set_json(resp, 200, root, NULL);
    return 0;
}

static int add_list_row(const mf_devinfo_t *d, void *arg)
{
    cJSON *arr = arg;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", d->uuid);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddStringToObject(o, "kind", d->kind);
    cJSON_AddStringToObject(o, "driver", d->driver);
    cJSON_AddBoolToObject(o, "online", d->online);
    cJSON_AddBoolToObject(o, "active", d->active);
    cJSON_AddItemToArray(arr, o);
    return 0;
}

static int handle_devices_list(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateArray();
    mf_devices_visit_live(add_list_row, root);
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_devices_create(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    cJSON *root;
    const char *name, *kind, *driver;
    char namebuf[32], kindbuf[16], driverbuf[16];
    char uuid[MF_UUID_LEN];
    char err[96];
    char *spec = NULL;
    char location[160];
    cJSON *reply;
    int rc;

    if (!req->body || req->body_len == 0)
    {
        set_error(resp, 400, "bad request");
        return 0;
    }
    root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root)
    {
        set_error(resp, 400, "bad request");
        return 0;
    }
    name = json_str(root, "name");
    kind = json_str(root, "kind");
    driver = json_str(root, "driver");
    if (!name || !kind || !driver)
    {
        set_error(resp, 400, "bad request");
        cJSON_Delete(root);
        return 0;
    }
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    snprintf(kindbuf, sizeof(kindbuf), "%s", kind);
    snprintf(driverbuf, sizeof(driverbuf), "%s", driver);
    spec = cJSON_PrintUnformatted(root);
    rc = mf_devices_add(namebuf, kindbuf, driverbuf, spec, NULL,
                        uuid, sizeof(uuid), err, sizeof(err));
    if (rc == 0)
        cfg_upsert_from_json(uuid, root);
    free(spec);
    cJSON_Delete(root);
    if (rc == -2)
    {
        set_error(resp, 400, "unknown kind/driver");
        return 0;
    }
    if (rc == -3)
    {
        set_error(resp, 400, "name in use");
        return 0;
    }
    if (rc == -4)
    {
        set_error(resp, 409, "device limit reached");
        return 0;
    }
    if (rc != 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }

    snprintf(location, sizeof(location), "/api/v1/devices/%s", uuid);
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "id", uuid);
    cJSON_AddStringToObject(reply, "name", namebuf);
    cJSON_AddStringToObject(reply, "kind", kindbuf);
    cJSON_AddStringToObject(reply, "driver", driverbuf);
    {
        mf_devinfo_t info;
        int on = 0;
        if (mf_devices_find_live(uuid, &info) == 0)
            on = info.online;
        cJSON_AddBoolToObject(reply, "online", on);
    }
    set_json(resp, 201, reply, location);
    return 0;
}

static int handle_device_get(const char *id, mf_rest_response_t *resp)
{
    mf_devinfo_t info;
    cJSON *obj;
    cJSON *data;

    if (mf_devices_find_live(id, &info) != 0)
    {
        set_error(resp, 404, "not found");
        return 0;
    }
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", info.uuid);
    cJSON_AddStringToObject(obj, "name", info.name);
    cJSON_AddStringToObject(obj, "kind", info.kind);
    cJSON_AddStringToObject(obj, "driver", info.driver);
    if (info.endpoint && info.endpoint[0])
        cJSON_AddStringToObject(obj, "endpoint", info.endpoint);
    cJSON_AddBoolToObject(obj, "online", info.online);
    cJSON_AddBoolToObject(obj, "active", info.active);
    cJSON_AddStringToObject(obj, "state", info.online ? "streaming" : "offline");
    cJSON_AddNumberToObject(obj, "seq", (double)info.seq);
    {
        cJSON *caps = cJSON_CreateArray();
        add_caps(caps, info.caps);
        cJSON_AddItemToObject(obj, "caps", caps);
    }
    data = cJSON_Parse(info.reading_json);
    if (data)
        cJSON_AddItemToObject(obj, "data", data);
    else
        cJSON_AddItemToObject(obj, "data", cJSON_CreateObject());
    set_json(resp, 200, obj, NULL);
    return 0;
}

static int handle_device_delete(const char *id, mf_rest_response_t *resp)
{
    int st = mf_devices_delete(id);
    if (st == 202)
    {
        cJSON *o = cJSON_CreateObject();
        cfg_remove_uuid(id);
        cJSON_AddStringToObject(o, "status", "stopping");
        set_json(resp, 202, o, NULL);
        return 0;
    }
    set_error(resp, 404, "not found");
    return 0;
}

static int handle_device_history(const char *id, mf_rest_response_t *resp)
{
    double values[800], ts[800];
    cJSON *root, *arr;
    int n, i;
    const char *column = "power_w";

    /* Pick column by device kind; fall back to power_w. */
    {
        mf_devinfo_t info;
        if (mf_devices_find_live(id, &info) == 0)
        {
            if (strcmp(info.kind, "battery") == 0)
                column = "soc";
        }
    }

    /* 60 s bins × 800 rows ≈ 13 h, still fits HTTP 64k. */
    n = mf_history_query_ts_step(id, column, 60, ts, values,
                                  (int)(sizeof(values) / sizeof(values[0])));
    if (n < 0)
    {
        set_error(resp, 404, "no history");
        return 0;
    }
    root = cJSON_CreateObject();
    if (!root)
    {
        set_error(resp, 500, "error");
        return 0;
    }
    cJSON_AddStringToObject(root, "column", column);
    arr = cJSON_AddArrayToObject(root, "ts");
    for (i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(ts[i]));
    arr = cJSON_AddArrayToObject(root, "values");
    for (i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(values[i]));
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_settings_get(const char *id, mf_rest_response_t *resp)
{
    char json[4096];
    int st = mf_devices_get_settings(id, json, sizeof(json));
    if (st != 200)
    {
        set_error(resp, st, st == 404 ? "not found" : "error");
        return 0;
    }
    {
        cJSON *root = cJSON_Parse(json);
        if (!root)
            root = cJSON_Parse("{}");
        set_json(resp, 200, root, NULL);
    }
    return 0;
}

static void copy_body(const mf_rest_request_t *req, char *dst, size_t cap)
{
    size_t n = req->body_len;
    if (!req->body)
        n = 0;
    if (n >= cap)
        n = cap - 1;
    if (n && req->body)
        memcpy(dst, req->body, n);
    dst[n] = '\0';
}

static int handle_settings_put(const char *id, const mf_rest_request_t *req,
                               mf_rest_response_t *resp)
{
    char err[96];
    char body[4096];
    int st;
    copy_body(req, body, sizeof(body));
    st = mf_devices_put_settings(id, body, err, sizeof(err));
    if (st != 200)
    {
        set_error(resp, st, err[0] ? err : (st == 404 ? "not found" : "bad request"));
        return 0;
    }
    {
        cJSON *b = cJSON_Parse(body);

        if (b)
        {
            cfg_patch_settings(id, b);
            cJSON_Delete(b);
        }
        if (g_cfg_path[0])
            (void)mf_config_save(live_cfg(), g_cfg_path);
        if (mf_config_save_overlay(live_cfg()) != 0)
            mf_log(LOG_WARNING, "settings overlay persist failed: %s",
                   mf_config_overlay_path());
    }
    return handle_settings_get(id, resp);
}

static int handle_action(const char *id, const char *action,
                         const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    char err[96];
    int st;

    char body[4096];
    copy_body(req, body, sizeof(body));
    mf_log(LOG_NOTICE, "action %s on %s from %s",
           action ? action : "?", id ? id : "?",
           req->peer && req->peer[0] ? req->peer : "-");
    st = mf_devices_action(id, action, body, err, sizeof(err));
    if (st == 200)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        set_json(resp, 200, o, NULL);
        return 0;
    }
    set_error(resp, st, err[0] ? err : (st == 404 ? "not found" : "bad request"));
    return 0;
}

static void set_etag_gen(mf_rest_response_t *resp, uint64_t gen)
{
    snprintf(resp->etag, sizeof(resp->etag), "\"%llu\"",
             (unsigned long long)gen);
}

static int gen_matches(const mf_rest_request_t *req, const cJSON *body,
                       uint64_t gen)
{
    uint64_t got = 0;
    int have = 0;

    if (req->if_match && req->if_match[0])
    {
        const char *p = req->if_match;
        while (*p == ' ' || *p == '"')
            p++;
        got = (uint64_t)strtoull(p, NULL, 10);
        have = 1;
    }
    if (body)
    {
        cJSON *g = cJSON_GetObjectItemCaseSensitive(body, "config_gen");
        if (cJSON_IsNumber(g))
        {
            got = (uint64_t)g->valuedouble;
            have = 1;
        }
    }
    return have && got == gen;
}

static int handle_config_get(mf_rest_response_t *resp)
{
    mf_daemon_config_t tmp;
    char *s;
    cJSON *root;
    mf_daemon_config_t *live = live_cfg();

    tmp = *live;
    mf_config_redact(&tmp);
    s = mf_config_serialize(&tmp);
    root = s ? cJSON_Parse(s) : cJSON_CreateObject();
    free(s);
    if (!root)
        root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "config_gen", (double)live->config_gen);
    cJSON_AddBoolToObject(root, "pending_apply", mf_devices_any_dying());
    set_json(resp, 200, root, NULL);
    set_etag_gen(resp, live->config_gen);
    return 0;
}

static int handle_config_put(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    mf_daemon_config_t *live = live_cfg();
    mf_daemon_config_t next;
    cJSON *root;
    char err[96];
    int rc;
    int listen_changed;

    if (!req->body || req->body_len == 0)
    {
        set_error(resp, 400, "bad request");
        return 0;
    }
    root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root)
    {
        set_error(resp, 400, "bad request");
        return 0;
    }
    if (!gen_matches(req, root, live->config_gen))
    {
        cJSON_Delete(root);
        set_error(resp, 409, "config_gen mismatch");
        return 0;
    }
    next = *live;
    mf_config_apply_json(&next, root);
    cJSON_Delete(root);
    if (mf_devices_validate_config(next.devices, next.n_devices,
                                   err, sizeof(err)) != 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }
    listen_changed = strcmp(next.listen, live->listen) != 0;
    if (listen_changed && mf_http_rebind_listen(next.listen) != 0)
    {
        set_error(resp, 500, "listen bind failed");
        return 0;
    }
    rc = mf_devices_apply_config(next.devices, next.n_devices,
                                  err, sizeof(err));
    if (rc < 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }
    next.config_gen = live->config_gen + 1;
    *live = next;
    if (rc == 1 || mf_devices_any_dying())
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "config_gen", (double)live->config_gen);
        cJSON_AddBoolToObject(o, "pending_apply", 1);
        set_json(resp, 202, o, NULL);
        set_etag_gen(resp, live->config_gen);
        return 0;
    }
    return handle_config_get(resp);
}

static int handle_config_save(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    mf_daemon_config_t *live = live_cfg();
    if (!gen_matches(req, NULL, live->config_gen))
    {
        set_error(resp, 409, "config_gen mismatch");
        return 0;
    }
    if (!g_cfg_path[0] || mf_config_save(live, g_cfg_path) != 0)
    {
        set_error(resp, 403, "not writable");
        return 0;
    }
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "saved");
        set_json(resp, 200, o, NULL);
        set_etag_gen(resp, live->config_gen);
    }
    return 0;
}

static int handle_config_load(mf_rest_response_t *resp)
{
    mf_daemon_config_t *live = live_cfg();
    mf_daemon_config_t next;
    char err[96];
    int rc;
    int listen_changed;

    if (!g_cfg_path[0])
    {
        set_error(resp, 404, "not found");
        return 0;
    }
    mf_config_defaults(&next);
    if (mf_config_load(g_cfg_path, &next) != 0)
    {
        set_error(resp, 404, "not found");
        return 0;
    }
    (void)mf_config_load_overlay(&next);
    if (mf_devices_validate_config(next.devices, next.n_devices,
                                   err, sizeof(err)) != 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }
    listen_changed = strcmp(next.listen, live->listen) != 0;
    if (listen_changed && mf_http_rebind_listen(next.listen) != 0)
    {
        set_error(resp, 500, "listen bind failed");
        return 0;
    }
    rc = mf_devices_apply_config(next.devices, next.n_devices,
                                  err, sizeof(err));
    if (rc < 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }
    next.config_gen = live->config_gen + 1;
    *live = next;
    if (rc == 1 || mf_devices_any_dying())
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "config_gen", (double)live->config_gen);
        cJSON_AddBoolToObject(o, "pending_apply", 1);
        set_json(resp, 202, o, NULL);
        set_etag_gen(resp, live->config_gen);
        return 0;
    }
    return handle_config_get(resp);
}

static int handle_discover_post(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    char err[96];
    int rc = mf_discover_start(req->body, req->body_len, err, sizeof(err));
    if (rc == -2)
    {
        set_error(resp, 409, err[0] ? err : "discover already running");
        return 0;
    }
    if (rc != 0)
    {
        set_error(resp, 400, err[0] ? err : "bad request");
        return 0;
    }
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "started");
        set_json(resp, 202, o, NULL);
    }
    return 0;
}

static int handle_discover_get(mf_rest_response_t *resp)
{
    char json[16384];
    cJSON *root;
    mf_discover_result(json, sizeof(json));
    root = cJSON_Parse(json);
    if (!root)
        root = cJSON_Parse("{\"status\":\"done\",\"results\":[]}");
    set_json(resp, 200, root, NULL);
    return 0;
}

static int method_is(const mf_rest_request_t *req, const char *m)
{
    return req->method && strcmp(req->method, m) == 0;
}

int mf_rest_dispatch(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    const char *p;
    const char *slash;

    if (!req || !req->path || !resp)
        return -1;
    memset(resp, 0, sizeof(*resp));

    if (strncmp(req->path, "/api/v1/", 8) != 0)
    {
        set_error(resp, 404, "not found");
        return 0;
    }
    p = req->path + 8;
    slash = strchr(p, '/');

    if (!slash)
    {
        if (strcmp(p, "status") == 0)
        {
            if (!method_is(req, "GET"))
            {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_status(resp);
        }
        if (strcmp(p, "drivers") == 0)
        {
            if (!method_is(req, "GET"))
            {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_drivers(resp);
        }
        if (strcmp(p, "devices") == 0)
        {
            if (method_is(req, "GET"))
                return handle_devices_list(resp);
            if (method_is(req, "POST"))
                return handle_devices_create(req, resp);
            set_error(resp, 405, "method not allowed");
            return 0;
        }
        if (strcmp(p, "health") == 0)
        {
            cJSON *root;
            if (!method_is(req, "GET"))
            {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", 1);
            cJSON_AddStringToObject(root, "server", "moonflared/" MF_VERSION);
            set_json(resp, 200, root, NULL);
            return 0;
        }
        if (strcmp(p, "config") == 0)
        {
            if (method_is(req, "GET"))
                return handle_config_get(resp);
            if (method_is(req, "PUT"))
                return handle_config_put(req, resp);
            set_error(resp, 405, "method not allowed");
            return 0;
        }
        if (strcmp(p, "discover") == 0)
        {
            if (method_is(req, "GET"))
                return handle_discover_get(resp);
            if (method_is(req, "POST"))
                return handle_discover_post(req, resp);
            set_error(resp, 405, "method not allowed");
            return 0;
        }
        set_error(resp, 404, "not found");
        return 0;
    }

    if (strncmp(p, "devices/", 8) == 0)
    {
        const char *id = p + 8;
        const char *slash2;
        char idbuf[MF_UUID_LEN];
        if (id[0] == '\0')
        {
            set_error(resp, 404, "not found");
            return 0;
        }
        slash2 = strchr(id, '/');
        if (slash2)
        {
            size_t n = (size_t)(slash2 - id);
            const char *rest = slash2 + 1;
            if (n >= sizeof(idbuf))
                n = sizeof(idbuf) - 1;
            memcpy(idbuf, id, n);
            idbuf[n] = '\0';
            if (strcmp(rest, "settings") == 0)
            {
                if (method_is(req, "GET"))
                    return handle_settings_get(idbuf, resp);
                if (method_is(req, "PUT"))
                    return handle_settings_put(idbuf, req, resp);
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            if (strcmp(rest, "history") == 0)
            {
                if (method_is(req, "GET"))
                    return handle_device_history(idbuf, resp);
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            if (strncmp(rest, "actions/", 8) == 0)
            {
                const char *action = rest + 8;
                if (!method_is(req, "POST"))
                {
                    set_error(resp, 405, "method not allowed");
                    return 0;
                }
                if (!action[0])
                {
                    set_error(resp, 404, "not found");
                    return 0;
                }
                return handle_action(idbuf, action, req, resp);
            }
            set_error(resp, 404, "not found");
            return 0;
        }
        snprintf(idbuf, sizeof(idbuf), "%s", id);
        if (method_is(req, "GET"))
            return handle_device_get(idbuf, resp);
        if (method_is(req, "DELETE"))
            return handle_device_delete(idbuf, resp);
        set_error(resp, 405, "method not allowed");
        return 0;
    }

    if (strncmp(p, "config/", 7) == 0)
    {
        const char *rest = p + 7;
        if (strcmp(rest, "save") == 0)
        {
            if (!method_is(req, "POST"))
            {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_config_save(req, resp);
        }
        if (strcmp(rest, "load") == 0)
        {
            if (!method_is(req, "POST"))
            {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_config_load(resp);
        }
        set_error(resp, 404, "not found");
        return 0;
    }

    set_error(resp, 404, "not found");
    return 0;
}

void mf_rest_init(void)
{
    (void)live_cfg();
}
