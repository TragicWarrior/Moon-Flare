#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "config/config.h"
#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ─── helpers ─────────────────────────────────────────────── */

static void cJSON_AddStringOrNull(cJSON *obj, const char *key, const char *val)
{
    if (val && val[0])
        cJSON_AddStringToObject(obj, key, val);
    else
        cJSON_AddNullToObject(obj, key);
}

static void cJSON_AddBool(cJSON *obj, const char *key, bool val)
{
    cJSON_AddBoolToObject(obj, key, val);
}

static void cJSON_AddNumber(cJSON *obj, const char *key, double val)
{
    cJSON_AddNumberToObject(obj, key, val);
}

/* ─── built-in defaults ─────────────────────────────────────── */

void mf_config_defaults(mf_daemon_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->listen, "0.0.0.0:5250", sizeof(cfg->listen) - 1);
    strncpy(cfg->plugin_dir, "/usr/local/lib/moon-flare",
            sizeof(cfg->plugin_dir) - 1);
    strncpy(cfg->gatt_bin, "/usr/local/libexec/mf_gatt",
            sizeof(cfg->gatt_bin) - 1);
    strncpy(cfg->history.path, "/var/lib/moonflare/history.sqlite",
            sizeof(cfg->history.path) - 1);
    cfg->history.enabled = true;
    cfg->n_devices     = 0;
    cfg->config_gen    = 1;
}

void mf_tui_config_defaults(mf_tui_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->connect, "127.0.0.1:5250", sizeof(cfg->connect) - 1);
    cfg->refresh_interval_s = 1.0;
}

/* ─── search-path resolution ────────────────────────────────── */

static char *resolve_in_dir(const char *dir, const char *filename)
{
    char path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        char *copy = strdup(path);
        if (!copy)
            return NULL;
        return copy;
    }
    return NULL;
}

char *mf_config_resolve_path(const char *override_path, const char *filename)
{
    /* 1. --config override (must exist). */
    if (override_path) {
        struct stat st;
        if (stat(override_path, &st) == 0 && S_ISREG(st.st_mode)) {
            return strdup(override_path);
        }
        return NULL; /* override given but missing — caller treats as "not found". */
    }

    /* 2. $HOME/.config/moonflare/ */
    {
        const char *home = getenv("HOME");
        if (home) {
            char dir[PATH_MAX];
            snprintf(dir, sizeof(dir), "%s/.config/moonflare", home);
            char *p = resolve_in_dir(dir, filename);
            if (p) return p;
        }
    }

    /* 3. /etc/moonflare/ */
    {
        char *p = resolve_in_dir("/etc/moonflare", filename);
        if (p) return p;
    }

    return NULL; /* fall through to built-in defaults */
}

/* ─── load (parse + apply) ──────────────────────────────────── */

static int load_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if ((size_t)fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    buf[(size_t)sz] = '\0';
    fclose(f);

    *out_buf = buf;
    if (out_len)
        *out_len = (size_t)sz;
    return 0;
}

int mf_config_load(const char *config_path_override, mf_daemon_config_t *cfg)
{
    mf_config_defaults(cfg);

    /* If explicit path given but missing → error (design: --config PATH must exist). */
    if (config_path_override) {
        struct stat st;
        if (stat(config_path_override, &st) < 0 || !S_ISREG(st.st_mode)) {
            return -1;
        }
    }

    char *path = mf_config_resolve_path(config_path_override,
                                        "moonflared.json");
    if (!path)
        return 0; /* no config found → defaults already set */

    char *raw = NULL;
    if (load_file(path, &raw, NULL) < 0) {
        free(path);
        return -1;
    }
    free(path);

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root)
        return -1;

    mf_config_apply_json(cfg, root);
    cJSON_Delete(root);
    return 0;
}

int mf_tui_config_load(const char *config_path_override, mf_tui_config_t *cfg)
{
    mf_tui_config_defaults(cfg);

    char *path = mf_config_resolve_path(config_path_override,
                                        "moonflare.json");
    if (!path)
        return 0;

    char *raw = NULL;
    if (load_file(path, &raw, NULL) < 0) {
        free(path);
        return -1;
    }
    free(path);

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root)
        return -1;

    mf_tui_config_apply_json(cfg, root);
    cJSON_Delete(root);
    return 0;
}

/* ─── apply JSON (unknown keys silently ignored) ─────────────── */

static void apply_history(mf_config_history_t *h, const cJSON *obj)
{
    if (!obj) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(obj, "path")) && v->type == cJSON_String)
        strncpy(h->path, v->valuestring, sizeof(h->path) - 1);
    if ((v = cJSON_GetObjectItem(obj, "enabled")) && v->type == cJSON_True)
        h->enabled = true;
}

static void apply_usb(mf_config_usb_t *u, const cJSON *obj)
{
    if (!obj) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(obj, "path")) && v->type == cJSON_String)
        strncpy(u->path, v->valuestring, sizeof(u->path) - 1);
    if ((v = cJSON_GetObjectItem(obj, "serial_id")) && v->type == cJSON_String)
        strncpy(u->serial_id, v->valuestring, sizeof(u->serial_id) - 1);
    if ((v = cJSON_GetObjectItem(obj, "by_id")) && v->type == cJSON_String)
        strncpy(u->by_id, v->valuestring, sizeof(u->by_id) - 1);
    if ((v = cJSON_GetObjectItem(obj, "auto_port")) && v->type == cJSON_True)
        u->auto_port = true;
    if ((v = cJSON_GetObjectItem(obj, "baud")) && v->type == cJSON_Number)
        u->baud = v->valueint;
    if ((v = cJSON_GetObjectItem(obj, "addr")) && v->type == cJSON_Number)
        u->addr = v->valueint;
}

static void apply_ble(mf_config_ble_t *b, const cJSON *obj)
{
    if (!obj) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(obj, "address")) && v->type == cJSON_String)
        strncpy(b->address, v->valuestring, sizeof(b->address) - 1);
    if ((v = cJSON_GetObjectItem(obj, "adapter")) && v->type == cJSON_String)
        strncpy(b->adapter, v->valuestring, sizeof(b->adapter) - 1);
    if ((v = cJSON_GetObjectItem(obj, "protocol")) && v->type == cJSON_String)
        strncpy(b->protocol, v->valuestring, sizeof(b->protocol) - 1);
    if ((v = cJSON_GetObjectItem(obj, "password")) && v->type == cJSON_String)
        strncpy(b->password, v->valuestring, sizeof(b->password) - 1);
    if ((v = cJSON_GetObjectItem(obj, "write_response")) && v->type == cJSON_True)
        b->write_response = true;
    if ((v = cJSON_GetObjectItem(obj, "connect_timeout_s")) && v->type == cJSON_Number)
        b->connect_timeout_s = v->valueint;
}

static void apply_modbus(mf_config_modbus_t *m, const cJSON *obj)
{
    if (!obj) return;
    /* Initialize to -1 (sentinel = not set). */
    memset(m, 0xff, sizeof(*m));
    cJSON *v;
    if ((v = cJSON_GetObjectItem(obj, "ip")) && v->type == cJSON_String)
        strncpy(m->ip, v->valuestring, sizeof(m->ip) - 1);
    if ((v = cJSON_GetObjectItem(obj, "port")) && v->type == cJSON_Number)
        m->port = v->valueint;
    if ((v = cJSON_GetObjectItem(obj, "unit_id")) && v->type == cJSON_Number)
        m->unit_id = v->valueint;
    if ((v = cJSON_GetObjectItem(obj, "mac")) && v->type == cJSON_String)
        strncpy(m->mac, v->valuestring, sizeof(m->mac) - 1);
    if ((v = cJSON_GetObjectItem(obj, "unit_device_id")) && v->type == cJSON_Number)
        m->unit_device_id = v->valueint;
    if ((v = cJSON_GetObjectItem(obj, "auto_net")) && v->type == cJSON_True)
        m->auto_net = true;
}

static int apply_device(mf_config_device_t *dev, const cJSON *obj)
{
    if (!obj || obj->type != cJSON_Object) return -1;

    /* UUID required for device entries. */
    cJSON *uuid = cJSON_GetObjectItem(obj, "uuid");
    if (!uuid || uuid->type != cJSON_String || !uuid->valuestring[0]) {
        fprintf(stderr, "config: device missing required 'uuid' field\n");
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    strncpy(dev->uuid, uuid->valuestring, sizeof(dev->uuid) - 1);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(obj, "name")) && v->type == cJSON_String)
        strncpy(dev->name, v->valuestring, sizeof(dev->name) - 1);
    if ((v = cJSON_GetObjectItem(obj, "kind")) && v->type == cJSON_String)
        strncpy(dev->kind, v->valuestring, sizeof(dev->kind) - 1);
    if ((v = cJSON_GetObjectItem(obj, "driver")) && v->type == cJSON_String)
        strncpy(dev->driver, v->valuestring, sizeof(dev->driver) - 1);
    if ((v = cJSON_GetObjectItem(obj, "enabled")) && v->type == cJSON_True)
        dev->enabled = true;
    if ((v = cJSON_GetObjectItem(obj, "poll_interval_s")) &&
        v->type == cJSON_Number)
        dev->poll_interval_s = v->valuedouble;
    dev->capture_interval_s = 10.0;
    if ((v = cJSON_GetObjectItem(obj, "capture_interval_s")) &&
        v->type == cJSON_Number)
        dev->capture_interval_s = v->valuedouble;
    if ((v = cJSON_GetObjectItem(obj, "bus")) && v->type == cJSON_String)
        strncpy(dev->bus, v->valuestring, sizeof(dev->bus) - 1);

    /* Nested objects (only applied if present). */
    if ((v = cJSON_GetObjectItem(obj, "usb")) && v->type == cJSON_Object)
        apply_usb(&dev->usb, v);
    if ((v = cJSON_GetObjectItem(obj, "ble")) && v->type == cJSON_Object)
        apply_ble(&dev->ble, v);
    if ((v = cJSON_GetObjectItem(obj, "modbus")) && v->type == cJSON_Object)
        apply_modbus(&dev->modbus, v);

    return 0;
}

int mf_config_device_from_json(mf_config_device_t *dev, const cJSON *obj)
{
    return apply_device(dev, obj);
}

void mf_config_apply_json(mf_daemon_config_t *cfg, const cJSON *root)
{
    if (!root || root->type != cJSON_Object) return;
    cJSON *v;

    if ((v = cJSON_GetObjectItem(root, "listen")) && v->type == cJSON_String)
        strncpy(cfg->listen, v->valuestring, sizeof(cfg->listen) - 1);
    if ((v = cJSON_GetObjectItem(root, "plugin_dir")) && v->type == cJSON_String)
        strncpy(cfg->plugin_dir, v->valuestring, sizeof(cfg->plugin_dir) - 1);
    if ((v = cJSON_GetObjectItem(root, "gatt_bin")) && v->type == cJSON_String)
        strncpy(cfg->gatt_bin, v->valuestring, sizeof(cfg->gatt_bin) - 1);
    if ((v = cJSON_GetObjectItem(root, "history")) && v->type == cJSON_Object)
        apply_history(&cfg->history, v);

    /* devices array — unknown keys at root level are silently ignored. */
    if ((v = cJSON_GetObjectItem(root, "devices")) &&
        v->type == cJSON_Array) {
        int n = cJSON_GetArraySize(v);
        cfg->n_devices = 0;
        for (int i = 0; i < n && cfg->n_devices < MF_MAX_DEVICES; i++) {
            cJSON *d = cJSON_GetArrayItem(v, i);
            if (apply_device(&cfg->devices[cfg->n_devices], d) == 0)
                cfg->n_devices++;
        }
    }
}

void mf_tui_config_apply_json(mf_tui_config_t *cfg, const cJSON *root)
{
    if (!root || root->type != cJSON_Object) return;
    cJSON *v;

    if ((v = cJSON_GetObjectItem(root, "connect")) && v->type == cJSON_String)
        strncpy(cfg->connect, v->valuestring, sizeof(cfg->connect) - 1);
    if ((v = cJSON_GetObjectItem(root, "refresh_interval_s")) &&
        v->type == cJSON_Number)
        cfg->refresh_interval_s = v->valuedouble;
}

/* ─── serialize ─────────────────────────────────────────────── */

cJSON *mf_config_deserialize_json(const char *json)
{
    return cJSON_Parse(json);
}

cJSON *mf_tui_config_deserialize_json(const char *json)
{
    return cJSON_Parse(json);
}

void mf_config_device_endpoint(const mf_config_device_t *d, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!d)
        return;
    if (d->usb.path[0]) {
        snprintf(buf, cap, "usb:%s", d->usb.path);
        return;
    }
    if (d->usb.serial_id[0]) {
        snprintf(buf, cap, "usb-id:%s", d->usb.serial_id);
        return;
    }
    if (d->ble.address[0]) {
        snprintf(buf, cap, "ble:%s", d->ble.address);
        return;
    }
    if (d->modbus.ip[0]) {
        int port = d->modbus.port > 0 ? d->modbus.port : 502;
        snprintf(buf, cap, "tcp:%s:%d", d->modbus.ip, port);
    }
}

static cJSON *device_to_json(const mf_config_device_t *d)
{
    cJSON *dev = cJSON_CreateObject();
    if (!dev)
        return NULL;
    cJSON_AddStringOrNull(dev, "uuid", d->uuid);
    cJSON_AddStringOrNull(dev, "name", d->name);
    cJSON_AddStringOrNull(dev, "kind", d->kind);
    cJSON_AddStringOrNull(dev, "driver", d->driver);
    cJSON_AddBool(dev, "enabled", d->enabled);
    cJSON_AddNumber(dev, "poll_interval_s", d->poll_interval_s);
    cJSON_AddNumber(dev, "capture_interval_s", d->capture_interval_s);
    cJSON_AddStringOrNull(dev, "bus", d->bus);
    {
        cJSON *usb = cJSON_CreateObject();
        cJSON_AddStringOrNull(usb, "path", d->usb.path);
        cJSON_AddStringOrNull(usb, "serial_id", d->usb.serial_id);
        cJSON_AddStringOrNull(usb, "by_id", d->usb.by_id);
        cJSON_AddBool(usb, "auto_port", d->usb.auto_port);
        cJSON_AddNumber(usb, "baud", d->usb.baud);
        cJSON_AddNumber(usb, "addr", d->usb.addr);
        cJSON_AddItemToObject(dev, "usb", usb);
    }
    {
        cJSON *ble = cJSON_CreateObject();
        cJSON_AddStringOrNull(ble, "address", d->ble.address);
        cJSON_AddStringOrNull(ble, "adapter", d->ble.adapter);
        cJSON_AddStringOrNull(ble, "protocol", d->ble.protocol);
        cJSON_AddStringOrNull(ble, "password", d->ble.password);
        cJSON_AddBool(ble, "write_response", d->ble.write_response);
        cJSON_AddNumber(ble, "connect_timeout_s",
                        d->ble.connect_timeout_s);
        cJSON_AddItemToObject(dev, "ble", ble);
    }
    {
        cJSON *mod = cJSON_CreateObject();
        cJSON_AddStringOrNull(mod, "ip", d->modbus.ip);
        cJSON_AddNumber(mod, "port", d->modbus.port);
        cJSON_AddNumber(mod, "unit_id", d->modbus.unit_id);
        cJSON_AddStringOrNull(mod, "mac", d->modbus.mac);
        cJSON_AddNumber(mod, "unit_device_id",
                        d->modbus.unit_device_id);
        cJSON_AddBool(mod, "auto_net", d->modbus.auto_net);
        cJSON_AddItemToObject(dev, "modbus", mod);
    }
    return dev;
}

char *mf_config_device_serialize(const mf_config_device_t *d)
{
    cJSON *dev;
    char *s;
    if (!d)
        return NULL;
    dev = device_to_json(d);
    if (!dev)
        return NULL;
    s = cJSON_PrintUnformatted(dev);
    cJSON_Delete(dev);
    return s;
}

char *mf_config_serialize(const mf_daemon_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddStringOrNull(root, "listen", cfg->listen);
    cJSON_AddStringOrNull(root, "plugin_dir", cfg->plugin_dir);
    cJSON_AddStringOrNull(root, "gatt_bin", cfg->gatt_bin);

    /* history */
    {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringOrNull(h, "path", cfg->history.path);
        cJSON_AddBool(h, "enabled", cfg->history.enabled);
        cJSON_AddItemToObject(root, "history", h);
    }

    /* devices */
    {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < cfg->n_devices; i++) {
            cJSON *dev = device_to_json(&cfg->devices[i]);
            if (dev)
                cJSON_AddItemToArray(arr, dev);
        }
        cJSON_AddItemToObject(root, "devices", arr);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

char *mf_tui_config_serialize(const mf_tui_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddStringOrNull(root, "connect", cfg->connect);
    cJSON_AddNumber(root, "refresh_interval_s", cfg->refresh_interval_s);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ─── atomic save ───────────────────────────────────────────── */

static void ensure_parent_dir(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *last = strrchr(tmp, '/');
    if (!last || last == tmp)
        return; /* nothing to create */
    *last = '\0';
    mkdir(tmp, 0755);
}

static int write_atomic(const char *path, const char *json)
{
    char tmp[PATH_MAX];
    size_t len;
    int fd;

    if (!path || !path[0] || !json)
        return -1;
    ensure_parent_dir(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    len = strlen(json);
    if (write(fd, json, len) != (ssize_t)len) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int mf_config_save(const mf_daemon_config_t *cfg, const char *path)
{
    char *json = mf_config_serialize(cfg);
    int rc;

    if (!json)
        return -1;
    rc = write_atomic(path, json);
    free(json);
    return rc;
}

/* ─── config_redact (strip passwords) ──────────────────────── */

void mf_config_redact(mf_daemon_config_t *cfg)
{
    for (int i = 0; i < cfg->n_devices; i++) {
        memset(cfg->devices[i].ble.password, 0,
               sizeof(cfg->devices[i].ble.password));
    }
}

/* ─── overlay merge ────────────────────────────────────────── */

/* Find a device index by uuid. Returns -1 if not found. */
static int find_device_by_uuid(const mf_daemon_config_t *cfg, const char *uuid)
{
    for (int i = 0; i < cfg->n_devices; i++)
        if (strcmp(cfg->devices[i].uuid, uuid) == 0)
            return i;
    return -1;
}

void mf_config_overlay_merge(mf_daemon_config_t *base_cfg,
                             const mf_daemon_config_t *overlay_cfg)
{
    for (int i = 0; i < overlay_cfg->n_devices; i++) {
        const mf_config_device_t *ov = &overlay_cfg->devices[i];
        int idx = find_device_by_uuid(base_cfg, ov->uuid);
        if (idx < 0)
            continue; /* uuid not in base — skip (overlay-only devices). */

        mf_config_device_t *base = &base_cfg->devices[idx];

        /* Overlay string fields if present (non-empty). */
        if (ov->name[0])
            strncpy(base->name, ov->name, sizeof(base->name) - 1);
        if (ov->kind[0])
            strncpy(base->kind, ov->kind, sizeof(base->kind) - 1);
        if (ov->driver[0])
            strncpy(base->driver, ov->driver, sizeof(base->driver) - 1);
        if (ov->bus[0])
            strncpy(base->bus, ov->bus, sizeof(base->bus) - 1);

        /* Numeric/bool fields. */
        if (ov->enabled)
            base->enabled = true;
        if (ov->poll_interval_s > 0)
            base->poll_interval_s = ov->poll_interval_s;

        /* USB overlay: only patch fields that are non-zero / non-empty in
         * overlay. */
        if (ov->usb.path[0])
            strncpy(base->usb.path, ov->usb.path,
                    sizeof(base->usb.path) - 1);
        if (ov->usb.serial_id[0])
            strncpy(base->usb.serial_id, ov->usb.serial_id,
                    sizeof(base->usb.serial_id) - 1);
        if (ov->usb.by_id[0])
            strncpy(base->usb.by_id, ov->usb.by_id,
                    sizeof(base->usb.by_id) - 1);
        if (ov->usb.baud > 0)
            base->usb.baud = ov->usb.baud;
        if (ov->usb.addr > 0)
            base->usb.addr = ov->usb.addr;
        if (ov->usb.auto_port)
            base->usb.auto_port = true;

        /* Modbus overlay. */
        if (ov->modbus.ip[0])
            strncpy(base->modbus.ip, ov->modbus.ip,
                    sizeof(base->modbus.ip) - 1);
        if (ov->modbus.port != -1)
            base->modbus.port = ov->modbus.port;
        if (ov->modbus.unit_id != -1)
            base->modbus.unit_id = ov->modbus.unit_id;
        if (ov->modbus.mac[0])
            strncpy(base->modbus.mac, ov->modbus.mac,
                    sizeof(base->modbus.mac) - 1);
        if (ov->modbus.unit_device_id != -1)
            base->modbus.unit_device_id = ov->modbus.unit_device_id;
        if (ov->modbus.auto_net)
            base->modbus.auto_net = true;
    }
}

const char *mf_config_overlay_path(void)
{
    static char path[256];
    const char *e = getenv("MF_SETTINGS_OVERLAY");
    const char *st;

    if (e && e[0])
        return e;
    st = getenv("STATE_DIRECTORY");
    if (!st || !st[0])
        st = "/var/lib/moonflare";
    snprintf(path, sizeof(path), "%s/settings.json", st);
    return path;
}

int mf_config_save_overlay(const mf_daemon_config_t *cfg)
{
    cJSON *root, *arr;
    char *json;
    int i, rc;

    if (!cfg)
        return -1;
    root = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "devices", arr);
    for (i = 0; i < cfg->n_devices; i++) {
        const mf_config_device_t *d = &cfg->devices[i];
        cJSON *o;

        if (!d->uuid[0])
            continue;
        o = cJSON_CreateObject();
        if (!o)
            continue;
        cJSON_AddStringToObject(o, "uuid", d->uuid);
        if (d->name[0])
            cJSON_AddStringToObject(o, "name", d->name);
        if (d->poll_interval_s > 0.0)
            cJSON_AddNumberToObject(o, "poll_interval_s", d->poll_interval_s);
        cJSON_AddNumberToObject(o, "capture_interval_s", d->capture_interval_s);
        cJSON_AddItemToArray(arr, o);
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return -1;
    rc = write_atomic(mf_config_overlay_path(), json);
    free(json);
    return rc;
}

int mf_config_load_overlay(mf_daemon_config_t *cfg)
{
    const char *path = mf_config_overlay_path();
    struct stat st;
    char *raw = NULL;
    cJSON *root;
    mf_daemon_config_t ov;

    if (!cfg || !path)
        return 0;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
        return 0;
    if (load_file(path, &raw, NULL) < 0)
        return -1;
    root = cJSON_Parse(raw);
    free(raw);
    if (!root)
        return -1;
    mf_config_defaults(&ov);
    mf_config_apply_json(&ov, root);
    cJSON_Delete(root);
    mf_config_overlay_merge(cfg, &ov);
    return 0;
}
