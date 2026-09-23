#include "mf_plugin.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCELL 16

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
    if ((size_t)n >= cap - *off)
    {
        *off = cap - 1;
        buf[cap - 1] = '\0';
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

typedef struct {
    char name[128];
    char uuid[64];
    int seed;
    unsigned frame_counter;
    double cell_v[NCELL];
    double mosfet_temp_c;
    int charge_on;
    int discharge_on;
    int balance_on;
} battery_ctx;

typedef struct {
    char name[128];
    char uuid[64];
    unsigned frame_counter;
    int charge_stage; /* 0=Resting 1=BulkMppt 2=Absorb 3=FloatMppt 4=Equalize */
} charger_ctx;

static const char *charge_stage_name(int s)
{
    static const char *names[] = {"Resting","BulkMppt","Absorb","FloatMppt","Equalize"};
    if (s >= 0 && s < (int)(sizeof(names)/sizeof(names[0])))
        return names[s];
    return "Resting";
}

static void *battery_open(const char *spec_json, char *err, size_t errsz)
{
    battery_ctx *ctx = (battery_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        if (err && errsz) snprintf(err, errsz, "oom");
        return NULL;
    }

    if (err && errsz) err[0] = '\0';

    /* Parse seed */
    const char *p = strstr(spec_json, "\"seed\"");
    if (p)
    {
        p = strchr(p, ':');
        if (p) ctx->seed = atoi(p + 1);
    }

    /* Parse name */
    p = strstr(spec_json, "\"name\"");
    if (p)
    {
        p = strchr(p, ':');
        if (p)
        {
            while (*++p == ' ' || *p == '"')
            {
            }
            const char *q = strchr(p, '"');
            if (q)
            {
                size_t n = (size_t)(q - p);
                if (n >= 127) n = 126;
                memcpy(ctx->name, p, n);
                ctx->name[n] = '\0';
            }
        }
    }

    /* Parse uuid */
    p = strstr(spec_json, "\"uuid\"");
    if (p)
    {
        p = strchr(p, ':');
        if (p)
        {
            while (*++p == ' ' || *p == '"')
            {
            }
            const char *q = strchr(p, '"');
            if (q)
            {
                size_t n = (size_t)(q - p);
                if (n >= 63) n = 62;
                memcpy(ctx->uuid, p, n);
                ctx->uuid[n] = '\0';
            }
        }
    }

    ctx->charge_on = 1;
    ctx->discharge_on = 1;
    ctx->balance_on = 0;
    return ctx;
}

static void battery_close(void *v)
{
    free(v);
}

static int battery_fd(void *v)
{
    (void)v;
    return -1;
}

static unsigned battery_select_mask(void *v)
{
    (void)v;
    return 0;
}

static mf_step_t battery_step(void *v)
{
    battery_ctx *ctx = (battery_ctx *)v;
    ctx->frame_counter++;
    return MF_STEP_IDLE;
}

static unsigned battery_caps(void *v)
{
    (void)v;
    return MF_CAP_READ | MF_CAP_ACTION_SWITCH;
}

static const char *battery_last_error(void *v)
{
    (void)v;
    return "";
}

static int battery_get_reading(void *v, char *json, size_t cap)
{
    battery_ctx *ctx = (battery_ctx *)v;
    double soc = 87.0;
    double current_a = 12.4;
    double t = (double)ctx->frame_counter;
    double pack_v = 0.0;
    size_t off = 0;
    int i;

    if (!ctx || !json || cap == 0)
        return -1;
    for (i = 0; i < NCELL; i++)
    {
        double wobble = 0.008 * sin(t / 8.0 + (double)i * 0.4);
        double bias = (i == 2) ? 0.012 : (i == 10) ? -0.010 : 0.0;
        ctx->cell_v[i] = 3.320 + wobble + bias + (soc - 50.0) * 0.0012
                         + (i == 0 ? (double)ctx->seed * 0.002 : 0.0);
        pack_v += ctx->cell_v[i];
    }
    ctx->mosfet_temp_c = 28.0 + 2.0 * sin(t / 20.0);

    js_append(json, cap, &off,
              "{\"pack_voltage_v\":%.4f,\"current_a\":%.2f,\"soc_pct\":%.1f,"
              "\"soh_pct\":98.0,\"cell_count\":%d,"
              "\"full_capacity_ah\":200.0,\"remaining_capacity_ah\":%.1f,"
              "\"charge_mosfet_on\":%s,"
              "\"discharge_mosfet_on\":%s,\"balancer_switch\":%s,"
              "\"cells\":[",
              pack_v, current_a, soc, NCELL, soc * 2.0,
              ctx->charge_on ? "true" : "false",
              ctx->discharge_on ? "true" : "false",
              ctx->balance_on ? "true" : "false");
    for (i = 0; i < NCELL; i++)
    {
        js_append(json, cap, &off,
                  "%s{\"index\":%d,\"voltage_v\":%.4f}",
                  i ? "," : "", i + 1, ctx->cell_v[i]);
    }
    js_append(json, cap, &off,
              "],\"temperatures_c\":[%.1f,%.1f,%.1f],"
              "\"temp_labels\":[\"MOS\",\"T1\",\"T2\"]}",
              ctx->mosfet_temp_c,
              24.0 + 1.0 * sin(t / 17.0),
              25.0 + 1.0 * sin(t / 19.0));
    return 0;
}

static int battery_get_settings(void *v, char *json, size_t cap)
{
    (void)v;
    return snprintf(json, cap, "{\"cell_count\":%d}", NCELL);
}

static int battery_put_settings(void *v, const char *json, char *err, size_t errsz)
{
    (void)v; (void)json;
    if (err && errsz) err[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}

static int battery_action(void *v, const char *action, const char *json,
                          char *err, size_t errsz)
{
    battery_ctx *ctx = (battery_ctx *)v;
    if (!ctx || !action)
    {
        if (err && errsz) snprintf(err, errsz, "invalid args");
        return MF_ERR_INVAL;
    }
    if (strcmp(action, "set_switch") == 0)
    {
        int on = 1;
        if (json && strstr(json, "false"))
            on = 0;
        if (json && strstr(json, "discharge"))
            ctx->discharge_on = on;
        else if (json && strstr(json, "balance"))
            ctx->balance_on = on;
        else
            ctx->charge_on = on;
        if (err && errsz)
            err[0] = '\0';
        return MF_OK;
    }
    if (strcmp(action, "refresh") == 0)
    {
        if (err && errsz) snprintf(err, errsz, "ok");
        return MF_OK;
    }
    if (err && errsz) snprintf(err, errsz, "unknown action: %s", action);
    return MF_ERR_UNSUPPORTED;
}

/* ── Charger ── */

static void *charger_open(const char *spec_json, char *err, size_t errsz)
{
    charger_ctx *ctx = (charger_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        if (err && errsz) snprintf(err, errsz, "oom");
        return NULL;
    }
    if (err && errsz) err[0] = '\0';

    const char *p = strstr(spec_json, "\"name\"");
    if (p)
    {
        p = strchr(p, ':');
        if (p)
        {
            while (*++p == ' ' || *p == '"')
            {
            }
            const char *q = strchr(p, '"');
            if (q)
            {
                size_t n = (size_t)(q - p);
                if (n > 126)
                    n = 126;
                memcpy(ctx->name, p, n);
                ctx->name[n] = '\0';
            }
        }
    }

    p = strstr(spec_json, "\"uuid\"");
    if (p)
    {
        p = strchr(p, ':');
        if (p)
        {
            while (*++p == ' ' || *p == '"')
            {
            }
            const char *q = strchr(p, '"');
            if (q)
            {
                size_t n = (size_t)(q - p);
                if (n > 62)
                    n = 62;
                memcpy(ctx->uuid, p, n);
                ctx->uuid[n] = '\0';
            }
        }
    }

    return ctx;
}

static void charger_close(void *v)
{
    free(v);
}

static int charger_fd(void *v)
{
    (void)v;
    return -1;
}
static unsigned charger_select_mask(void *v)
{
    (void)v;
    return 0;
}

static mf_step_t charger_step(void *v)
{
    charger_ctx *ctx = (charger_ctx *)v;
    ctx->frame_counter++;
    /* Rotate charge stage every 10 ticks */
    ctx->charge_stage = (int)((ctx->frame_counter / 10u) % 5u);
    return MF_STEP_IDLE;
}

static unsigned charger_caps(void *v)
{
    (void)v;
    return MF_CAP_READ;
}

static const char *charger_last_error(void *v)
{
    (void)v;
    return "";
}

static int charger_get_reading(void *v, char *json, size_t cap)
{
    charger_ctx *ctx = (charger_ctx *)v;
    if (!ctx || !json || cap == 0)
        return -1;
    snprintf(json, cap,
             "{\"battery_voltage_v\":54.1,\"charging_watts\":840,"
             "\"kwh_today\":3.2,\"charge_stage\":\"%s\"}",
             charge_stage_name(ctx->charge_stage));
    return 0;
}

static int charger_get_settings(void *v, char *json, size_t cap)
{
    (void)v;
    return snprintf(json, cap, "{\"max_current_a\":50}");
}

static int charger_put_settings(void *v, const char *json, char *err, size_t errsz)
{
    (void)v; (void)json;
    if (err && errsz) err[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}

static int charger_action(void *v, const char *action, const char *json,
                          char *err, size_t errsz)
{
    (void)v; (void)action; (void)json;
    if (err && errsz) snprintf(err, errsz, "unsupported");
    return MF_ERR_UNSUPPORTED;
}

/* ── Plugin entries ── */

static const mf_plugin_ops_t g_ops[2] = {
    {
        .abi         = MF_PLUGIN_ABI,
        .ops_size    = sizeof(mf_plugin_ops_t),
        .kind        = "battery",
        .driver      = "demo",
        .version     = "1.0.0",
        .open        = battery_open,
        .close       = battery_close,
        .fd          = battery_fd,
        .select_mask = battery_select_mask,
        .prepare_fds = NULL,
        .step        = battery_step,
        .caps        = battery_caps,
        .last_error  = battery_last_error,
        .get_reading = battery_get_reading,
        .get_settings = battery_get_settings,
        .put_settings = battery_put_settings,
        .action      = battery_action,
        .probe_start = NULL,
        .probe_step  = NULL,
        .probe_select_mask = NULL,
        .probe_prepare_fds = NULL,
        .probe_result = NULL,
        .probe_close = NULL,
    },
    {
        .abi         = MF_PLUGIN_ABI,
        .ops_size    = sizeof(mf_plugin_ops_t),
        .kind        = "charger",
        .driver      = "demo",
        .version     = "1.0.0",
        .open        = charger_open,
        .close       = charger_close,
        .fd          = charger_fd,
        .select_mask = charger_select_mask,
        .prepare_fds = NULL,
        .step        = charger_step,
        .caps        = charger_caps,
        .last_error  = charger_last_error,
        .get_reading = charger_get_reading,
        .get_settings = charger_get_settings,
        .put_settings = charger_put_settings,
        .action      = charger_action,
        .probe_start = NULL,
        .probe_step  = NULL,
        .probe_select_mask = NULL,
        .probe_prepare_fds = NULL,
        .probe_result = NULL,
        .probe_close = NULL,
    },
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out) *out = g_ops;
    return 2;
}
