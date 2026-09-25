/*
 * Built-in phantom driver (kind battery / charger / inverter).
 *
 * Settings: "phantom.shadows", the UUIDs of the real modules it shadows,
 * comma-separated (the config keeps it as {"phantom": {"shadows": "..."}}).
 * Only shadowed modules that are both active and online are averaged; with
 * none, the phantom is offline and says why.  The shadows are looked up on
 * every poll, so the order modules start in does not matter and a removed
 * one simply drops out.
 */

#include "phantom.h"
#include "average.h"
#include "device.h"

#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SHADOWS 8
#define POLL_DEFAULT_S 2.0
#define POLL_MIN_S 0.5

typedef struct {
    char   uuid[MF_UUID_LEN];           /* the phantom's own */
    char   kind[16];
    char   shadows[MAX_SHADOWS][MF_UUID_LEN];
    int    nshadows;
    double poll_s;
    double next;
    char  *reading;                     /* last average, printed */
    char   err[96];
} ph_ctx_t;

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int is_uuid(const char *s)
{
    size_t i, n = strlen(s);

    if (n < 8 || n >= MF_UUID_LEN)
        return 0;
    for (i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i]) && s[i] != '-')
            return 0;
    return 1;
}

/* "a,b" (or a JSON array of strings) into out[]; 0 ok, -1 malformed. */
static int parse_shadows(const cJSON *v, char out[][MF_UUID_LEN], int *n,
                         char *err, size_t errsz)
{
    char buf[MAX_SHADOWS * MF_UUID_LEN + MAX_SHADOWS], *tok, *save = NULL;

    *n = 0;
    if (cJSON_IsArray(v))
    {
        const cJSON *it;
        size_t off = 0;

        buf[0] = '\0';
        cJSON_ArrayForEach(it, v)
            if (cJSON_IsString(it) && off < sizeof(buf) - 1)
                off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s",
                                        off ? "," : "", it->valuestring);
    }
    else if (cJSON_IsString(v))
        snprintf(buf, sizeof(buf), "%s", v->valuestring);
    else if (!v || cJSON_IsNull(v))
        buf[0] = '\0';
    else
    {
        snprintf(err, errsz, "phantom.shadows must be a list of module ids");
        return -1;
    }
    for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
    {
        int k, dup = 0;

        if (!is_uuid(tok))
        {
            snprintf(err, errsz, "phantom.shadows: '%.40s' is not a module id", tok);
            return -1;
        }
        for (k = 0; k < *n; k++)
            dup |= strcmp(out[k], tok) == 0;
        if (dup)
            continue;
        if (*n >= MAX_SHADOWS)
        {
            snprintf(err, errsz, "phantom.shadows: at most %d modules", MAX_SHADOWS);
            return -1;
        }
        snprintf(out[(*n)++], MF_UUID_LEN, "%s", tok);
    }
    return 0;
}

static void *ph_open(const char *spec_json, char *err, size_t errsz)
{
    cJSON *spec = spec_json ? cJSON_Parse(spec_json) : NULL;
    const cJSON *it;
    ph_ctx_t *c = calloc(1, sizeof(*c));

    if (!c || !cJSON_IsObject(spec))
    {
        snprintf(err, errsz, c ? "bad module config" : "out of memory");
        free(c);
        cJSON_Delete(spec);
        return NULL;
    }
    it = cJSON_GetObjectItemCaseSensitive(spec, "uuid");
    if (cJSON_IsString(it))
        snprintf(c->uuid, sizeof(c->uuid), "%s", it->valuestring);
    it = cJSON_GetObjectItemCaseSensitive(spec, "kind");
    snprintf(c->kind, sizeof(c->kind), "%s",
             cJSON_IsString(it) ? it->valuestring : "");
    it = cJSON_GetObjectItemCaseSensitive(spec, "poll_interval_s");
    c->poll_s = cJSON_IsNumber(it) && it->valuedouble >= POLL_MIN_S
                ? it->valuedouble : POLL_DEFAULT_S;
    /* Config nests plugin keys; an add request may still send them dotted. */
    it = cJSON_GetObjectItemCaseSensitive(
             cJSON_GetObjectItemCaseSensitive(spec, "phantom"), "shadows");
    if (!it)
        it = cJSON_GetObjectItemCaseSensitive(spec, "phantom.shadows");
    if (parse_shadows(it, c->shadows, &c->nshadows, err, errsz) != 0)
    {
        free(c);
        cJSON_Delete(spec);
        return NULL;
    }
    cJSON_Delete(spec);
    return c;
}

static void ph_close(void *ctx)
{
    ph_ctx_t *c = ctx;

    if (!c)
        return;
    free(c->reading);
    free(c);
}

static int ph_fd(void *ctx)
{
    (void)ctx;
    return -1;
}

static unsigned ph_mask(void *ctx)
{
    (void)ctx;
    return 0;
}

/* Average the shadows that are active and online, every poll interval. */
static mf_step_t ph_step(void *ctx)
{
    ph_ctx_t *c = ctx;
    const cJSON *readings[MAX_SHADOWS];
    cJSON *parsed[MAX_SHADOWS], *avg, *names;
    char why[96];
    double t = now_mono();
    int i, n = 0;

    if (t < c->next)
        return MF_STEP_IDLE;
    c->next = t + c->poll_s;
    why[0] = '\0';
    names = cJSON_CreateArray();
    for (i = 0; i < c->nshadows; i++)
    {
        mf_devinfo_t info;
        cJSON *r;

        if (mf_devices_find_live(c->shadows[i], &info) != 0)
            continue;                   /* removed, or not started yet */
        if (strcmp(info.kind, c->kind) != 0 ||
            strcmp(info.driver, MF_PHANTOM_DRIVER) == 0)
            continue;
        if (!info.active || !info.online)
        {
            snprintf(why, sizeof(why), "%s is %s", info.name,
                     !info.active ? "inactive" : "offline");
            continue;
        }
        r = cJSON_Parse(info.reading_json);
        if (!cJSON_IsObject(r) || !r->child)
        {
            cJSON_Delete(r);
            continue;
        }
        parsed[n] = r;
        readings[n] = r;
        n++;
        cJSON_AddItemToArray(names, cJSON_CreateString(info.name));
    }
    if (n == 0)
    {
        /* Offline until a shadow qualifies again: no stale average. */
        free(c->reading);
        c->reading = NULL;
        cJSON_Delete(names);
        if (c->nshadows == 0)
            snprintf(c->err, sizeof(c->err), "no modules to shadow");
        else if (why[0])
            snprintf(c->err, sizeof(c->err), "shadowed %s", why);
        else
            snprintf(c->err, sizeof(c->err), "no shadowed module is running");
        return MF_STEP_ERROR;
    }
    avg = mf_phantom_average(readings, n);
    for (i = 0; i < n; i++)
        cJSON_Delete(parsed[i]);
    if (!avg)
    {
        cJSON_Delete(names);
        snprintf(c->err, sizeof(c->err), "out of memory");
        return MF_STEP_ERROR;
    }
    cJSON_AddItemToObject(avg, "phantom_of", names);
    free(c->reading);
    c->reading = cJSON_PrintUnformatted(avg);
    cJSON_Delete(avg);
    c->err[0] = '\0';
    return c->reading ? MF_STEP_UPDATED : MF_STEP_ERROR;
}

static unsigned ph_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS;
}

static const char *ph_last_error(void *ctx)
{
    ph_ctx_t *c = ctx;

    return c && c->err[0] ? c->err : NULL;
}

static int ph_get_reading(void *ctx, char *json, size_t cap)
{
    ph_ctx_t *c = ctx;

    if (!c || !c->reading)
        return MF_ERR_OFFLINE;
    if (strlen(c->reading) >= cap)
        return MF_ERR_INVAL;
    memcpy(json, c->reading, strlen(c->reading) + 1);
    return MF_OK;
}

static int ph_get_settings(void *ctx, char *json, size_t cap)
{
    ph_ctx_t *c = ctx;
    size_t off;
    int i;

    off = (size_t)snprintf(json, cap, "{\"phantom.shadows\":\"");
    for (i = 0; i < c->nshadows && off < cap; i++)
        off += (size_t)snprintf(json + off, cap - off, "%s%s",
                                i ? "," : "", c->shadows[i]);
    if (off < cap)
        snprintf(json + off, cap - off, "\"}");
    return off + 2 < cap ? MF_OK : MF_ERR_INVAL;
}

/* Only real modules of the same kind can be shadowed, never itself. */
static int ph_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    ph_ctx_t *c = ctx;
    cJSON *b = json ? cJSON_Parse(json) : NULL;
    const cJSON *v;
    char list[MAX_SHADOWS][MF_UUID_LEN];
    int n, i;

    if (!cJSON_IsObject(b))
    {
        cJSON_Delete(b);
        snprintf(err, errsz, "settings are not a JSON object");
        return MF_ERR_INVAL;
    }
    v = cJSON_GetObjectItemCaseSensitive(b, "poll_interval_s");
    if (cJSON_IsNumber(v) && v->valuedouble >= POLL_MIN_S)
        c->poll_s = v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(b, "phantom.shadows");
    if (!v)
    {
        cJSON_Delete(b);
        return MF_OK;
    }
    if (parse_shadows(v, list, &n, err, errsz) != 0)
    {
        cJSON_Delete(b);
        return MF_ERR_INVAL;
    }
    cJSON_Delete(b);
    for (i = 0; i < n; i++)
    {
        mf_devinfo_t info;

        if (strcmp(list[i], c->uuid) == 0)
        {
            snprintf(err, errsz, "a phantom cannot shadow itself");
            return MF_ERR_INVAL;
        }
        if (mf_devices_find_live(list[i], &info) != 0)
        {
            snprintf(err, errsz, "no module %.40s", list[i]);
            return MF_ERR_INVAL;
        }
        if (strcmp(info.driver, MF_PHANTOM_DRIVER) == 0)
        {
            snprintf(err, errsz, "%s is a phantom; shadow real modules",
                     info.name);
            return MF_ERR_INVAL;
        }
        if (strcmp(info.kind, c->kind) != 0)
        {
            snprintf(err, errsz, "%s is a %s, not a %s", info.name, info.kind,
                     c->kind);
            return MF_ERR_INVAL;
        }
    }
    memcpy(c->shadows, list, sizeof(list));
    c->nshadows = n;
    c->next = 0.0;                      /* recompute on the next tick */
    return MF_OK;
}

static int ph_action(void *ctx, const char *action, const char *json,
                     char *err, size_t errsz)
{
    (void)ctx;
    (void)json;
    snprintf(err, errsz, "a phantom has no actions (%s)", action ? action : "");
    return MF_ERR_UNSUPPORTED;
}

/* No "capture" block: a phantom records no history. */
static const char *ph_describe(void)
{
    return
        "{\"bus\":\"phantom\",\"fields\":["
        "{\"key\":\"poll_interval_s\",\"label\":\"Poll Interval\","
        "\"hint\":\"(Seconds)\",\"type\":\"number\",\"default\":2},"
        "{\"key\":\"phantom.shadows\",\"label\":\"Shadows\","
        "\"hint\":\"(modules)\",\"type\":\"modules\",\"required\":true}"
        "]}";
}

#define PHANTOM_OPS(k) {                                          \
        .abi = MF_PLUGIN_ABI, .ops_size = sizeof(mf_plugin_ops_t), \
        .kind = (k), .driver = MF_PHANTOM_DRIVER,                  \
        .version = "1.0.0",                                        \
        .open = ph_open, .close = ph_close,                        \
        .fd = ph_fd, .select_mask = ph_mask, .step = ph_step,      \
        .caps = ph_caps, .last_error = ph_last_error,              \
        .get_reading = ph_get_reading,                             \
        .get_settings = ph_get_settings,                           \
        .put_settings = ph_put_settings,                           \
        .action = ph_action,                                       \
        .describe = ph_describe }

static const mf_plugin_ops_t g_phantom_ops[] = {
    PHANTOM_OPS("battery"),
    PHANTOM_OPS("charger"),
    PHANTOM_OPS("inverter"),
};

int mf_phantom_register(mf_plugin_registry_t *reg)
{
    size_t i;
    int n = 0;

    for (i = 0; i < sizeof(g_phantom_ops) / sizeof(g_phantom_ops[0]); i++)
        if (mf_plugins_add_builtin(reg, &g_phantom_ops[i]) == 0)
            n++;
    return n;
}
