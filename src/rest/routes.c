#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "rest.h"
#include "device.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (s) {
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
    if (s) {
        size_t n = strlen(s);
        if (n >= sizeof(resp->body))
            n = sizeof(resp->body) - 1;
        memcpy(resp->body, s, n);
        resp->body[n] = '\0';
        resp->body_len = n;
        free(s);
    }
    if (location && location[0]) {
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

static int add_status_row(const mf_devinfo_t *d, void *arg)
{
    cJSON *root = arg;
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
    cJSON_AddNumberToObject(row, "seq", (double)d->seq);

    if (strcmp(d->kind, "charger") == 0) {
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
        cJSON_AddItemToArray(chargers, row);
    } else if (strcmp(d->kind, "inverter") == 0) {
        cJSON_AddItemToArray(inverters, row);
    } else if (strcmp(d->kind, "phantom") == 0) {
        cJSON_AddItemToArray(phantoms, row);
    } else {
        cJSON *v = num_or_null(data, "pack_voltage_v");
        cJSON *c = num_or_null(data, "current_a");
        cJSON *s = num_or_null(data, "soc_pct");
        cJSON *n = num_or_null(data, "cell_count");
        if (v)
            cJSON_AddNumberToObject(row, "pack_voltage_v", v->valuedouble);
        if (c)
            cJSON_AddNumberToObject(row, "current_a", c->valuedouble);
        if (s)
            cJSON_AddNumberToObject(row, "soc_pct", s->valuedouble);
        if (n)
            cJSON_AddNumberToObject(row, "cell_count", n->valuedouble);
        cJSON_AddItemToArray(batteries, row);
    }
    if (data)
        cJSON_Delete(data);
    return 0;
}

static int handle_status(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "server", "moonflared/0.1.0");
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddItemToObject(root, "batteries", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "chargers", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "inverters", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "phantoms", cJSON_CreateArray());
    mf_devices_visit_live(add_status_row, root);
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_drivers(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    const mf_plugin_registry_t *reg = mf_devices_registry();
    int i;

    if (reg && reg->nops > 0) {
        for (i = 0; i < reg->nops; i++) {
            cJSON *o = cJSON_CreateObject();
            const mf_plugin_ops_t *ops = reg->ops[i];
            cJSON_AddStringToObject(o, "kind", ops->kind ? ops->kind : "");
            cJSON_AddStringToObject(o, "driver", ops->driver ? ops->driver : "");
            cJSON_AddItemToArray(arr, o);
        }
    } else {
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

    if (!req->body || req->body_len == 0) {
        set_error(resp, 400, "bad request");
        return 0;
    }
    root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        set_error(resp, 400, "bad request");
        return 0;
    }
    name = json_str(root, "name");
    kind = json_str(root, "kind");
    driver = json_str(root, "driver");
    if (!name || !kind || !driver) {
        set_error(resp, 400, "bad request");
        cJSON_Delete(root);
        return 0;
    }
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    snprintf(kindbuf, sizeof(kindbuf), "%s", kind);
    snprintf(driverbuf, sizeof(driverbuf), "%s", driver);
    spec = cJSON_PrintUnformatted(root);
    rc = mf_devices_add(namebuf, kindbuf, driverbuf, spec, uuid, sizeof(uuid),
                        err, sizeof(err));
    free(spec);
    cJSON_Delete(root);
    if (rc == -2) {
        set_error(resp, 400, "unknown kind/driver");
        return 0;
    }
    if (rc == -3) {
        set_error(resp, 400, "name in use");
        return 0;
    }
    if (rc == -4) {
        set_error(resp, 409, "device limit reached");
        return 0;
    }
    if (rc != 0) {
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

static int handle_device_get(const char *id, mf_rest_response_t *resp)
{
    mf_devinfo_t info;
    cJSON *obj;
    cJSON *data;

    if (mf_devices_find_live(id, &info) != 0) {
        set_error(resp, 404, "not found");
        return 0;
    }
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", info.uuid);
    cJSON_AddStringToObject(obj, "name", info.name);
    cJSON_AddStringToObject(obj, "kind", info.kind);
    cJSON_AddStringToObject(obj, "driver", info.driver);
    cJSON_AddBoolToObject(obj, "online", info.online);
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
    if (st == 202) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "stopping");
        set_json(resp, 202, o, NULL);
        return 0;
    }
    set_error(resp, 404, "not found");
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

    if (strncmp(req->path, "/api/v1/", 8) != 0) {
        set_error(resp, 404, "not found");
        return 0;
    }
    p = req->path + 8;
    slash = strchr(p, '/');

    if (!slash) {
        if (strcmp(p, "status") == 0) {
            if (!method_is(req, "GET")) {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_status(resp);
        }
        if (strcmp(p, "drivers") == 0) {
            if (!method_is(req, "GET")) {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            return handle_drivers(resp);
        }
        if (strcmp(p, "devices") == 0) {
            if (method_is(req, "GET"))
                return handle_devices_list(resp);
            if (method_is(req, "POST"))
                return handle_devices_create(req, resp);
            set_error(resp, 405, "method not allowed");
            return 0;
        }
        if (strcmp(p, "health") == 0) {
            cJSON *root;
            if (!method_is(req, "GET")) {
                set_error(resp, 405, "method not allowed");
                return 0;
            }
            root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", 1);
            cJSON_AddStringToObject(root, "server", "moonflared/0.1.0");
            set_json(resp, 200, root, NULL);
            return 0;
        }
        set_error(resp, 404, "not found");
        return 0;
    }

    if (strncmp(p, "devices/", 8) == 0) {
        const char *id = p + 8;
        const char *slash2;
        char idbuf[MF_UUID_LEN];
        if (id[0] == '\0') {
            set_error(resp, 404, "not found");
            return 0;
        }
        slash2 = strchr(id, '/');
        if (slash2) {
            /* /devices/{id}/settings|actions — PR-10 */
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

    set_error(resp, 404, "not found");
    return 0;
}

void mf_rest_init(void)
{
    /* Device table is owned by mf_devices_init(). */
}
