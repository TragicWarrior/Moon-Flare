#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "rest.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEVICES 32
#define UUID_LEN    37

typedef struct {
    char uuid[UUID_LEN];
    char name[64];
    char kind[32];
    char driver[32];
} device_t;

static device_t g_devices[MAX_DEVICES];
static int      g_dev_count;

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

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it) || !it->valuestring)
        return NULL;
    return it->valuestring;
}

static const device_t *find_device(const char *uuid)
{
    int i;
    for (i = 0; i < g_dev_count; i++) {
        if (strcmp(g_devices[i].uuid, uuid) == 0)
            return &g_devices[i];
    }
    return NULL;
}

static int handle_status(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *batteries = cJSON_CreateArray();
    cJSON *chargers = cJSON_CreateArray();
    int i;

    cJSON_AddStringToObject(root, "server", "moonflared/0.1.0");
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    for (i = 0; i < g_dev_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", g_devices[i].uuid);
        cJSON_AddStringToObject(d, "name", g_devices[i].name);
        cJSON_AddStringToObject(d, "driver", g_devices[i].driver);
        cJSON_AddBoolToObject(d, "online", 0);
        if (strcmp(g_devices[i].kind, "charger") == 0)
            cJSON_AddItemToArray(chargers, d);
        else
            cJSON_AddItemToArray(batteries, d);
    }
    cJSON_AddItemToObject(root, "batteries", batteries);
    cJSON_AddItemToObject(root, "chargers", chargers);
    cJSON_AddItemToObject(root, "inverters", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "phantoms", cJSON_CreateArray());
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_drivers(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON *b = cJSON_CreateObject();
    cJSON *c = cJSON_CreateObject();

    cJSON_AddStringToObject(b, "kind", "battery");
    cJSON_AddStringToObject(b, "driver", "demo");
    cJSON_AddItemToArray(arr, b);
    cJSON_AddStringToObject(c, "kind", "charger");
    cJSON_AddStringToObject(c, "driver", "demo");
    cJSON_AddItemToArray(arr, c);
    cJSON_AddItemToObject(root, "drivers", arr);
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_devices_list(mf_rest_response_t *resp)
{
    cJSON *root = cJSON_CreateArray();
    int i;
    for (i = 0; i < g_dev_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", g_devices[i].uuid);
        cJSON_AddStringToObject(d, "name", g_devices[i].name);
        cJSON_AddStringToObject(d, "kind", g_devices[i].kind);
        cJSON_AddStringToObject(d, "driver", g_devices[i].driver);
        cJSON_AddBoolToObject(d, "online", 0);
        cJSON_AddItemToArray(root, d);
    }
    set_json(resp, 200, root, NULL);
    return 0;
}

static int handle_devices_create(const mf_rest_request_t *req, mf_rest_response_t *resp)
{
    cJSON *root;
    const char *name;
    const char *kind;
    const char *driver;
    device_t *d;
    char location[160];
    cJSON *reply;

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
    if (g_dev_count >= MAX_DEVICES) {
        set_error(resp, 409, "device limit reached");
        cJSON_Delete(root);
        return 0;
    }
    d = &g_devices[g_dev_count];
    memset(d, 0, sizeof(*d));
    uuid_generate(d->uuid, sizeof(d->uuid));
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->kind, sizeof(d->kind), "%s", kind);
    snprintf(d->driver, sizeof(d->driver), "%s", driver);
    g_dev_count++;

    snprintf(location, sizeof(location), "/api/v1/devices/%s", d->uuid);
    reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "id", d->uuid);
    cJSON_AddStringToObject(reply, "name", d->name);
    cJSON_AddStringToObject(reply, "kind", d->kind);
    cJSON_AddStringToObject(reply, "driver", d->driver);
    cJSON_AddBoolToObject(reply, "online", 0);
    set_json(resp, 201, reply, location);
    cJSON_Delete(root);
    return 0;
}

static int handle_device_get(const char *id, mf_rest_response_t *resp)
{
    const device_t *d = find_device(id);
    cJSON *obj;

    if (!d) {
        set_error(resp, 404, "not found");
        return 0;
    }
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", d->uuid);
    cJSON_AddStringToObject(obj, "name", d->name);
    cJSON_AddStringToObject(obj, "kind", d->kind);
    cJSON_AddStringToObject(obj, "driver", d->driver);
    cJSON_AddBoolToObject(obj, "online", 0);
    cJSON_AddStringToObject(obj, "state", "offline");
    set_json(resp, 200, obj, NULL);
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
        if (id[0] == '\0') {
            set_error(resp, 404, "not found");
            return 0;
        }
        if (!method_is(req, "GET")) {
            set_error(resp, 405, "method not allowed");
            return 0;
        }
        return handle_device_get(id, resp);
    }

    set_error(resp, 404, "not found");
    return 0;
}

void mf_rest_init(void)
{
    g_dev_count = 0;
    memset(g_devices, 0, sizeof(g_devices));
}
