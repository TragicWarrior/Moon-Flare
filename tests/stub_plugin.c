/* Tiny libmf_*.so for loader tests. Not the demo driver. */
#include "mf_plugin.h"

#include <stdio.h>
#include <string.h>

static void *stub_open(const char *spec_json, char *err, size_t errsz)
{
    (void)spec_json;
    if (err && errsz)
        err[0] = '\0';
    return (void *)0x1;
}

static void stub_close(void *ctx)
{
    (void)ctx;
}
static int stub_fd(void *ctx)
{
    (void)ctx;
    return -1;
}
static unsigned stub_mask(void *ctx)
{
    (void)ctx;
    return 0;
}
static mf_step_t stub_step(void *ctx)
{
    (void)ctx;
    return MF_STEP_IDLE;
}
static unsigned stub_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ;
}
static const char *stub_err(void *ctx)
{
    (void)ctx;
    return "";
}
static int stub_reading(void *ctx, char *json, size_t cap)
{
    (void)ctx;
    if (!json || cap == 0)
        return -1;
    snprintf(json, cap, "{\"ok\":true}");
    return 0;
}

static int stub_get_settings(void *ctx, char *json, size_t cap)
{
    (void)ctx;
    if (json && cap)
        json[0] = '\0';
    return 0;
}
static int stub_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    (void)ctx;
    (void)json;
    if (err && errsz)
        err[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}
static int stub_action(void *ctx, const char *a, const char *json, char *err, size_t errsz)
{
    (void)ctx;
    (void)a;
    (void)json;
    if (err && errsz)
        err[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}
static int stub_probe_start(const char *args, void **job, char *err, size_t errsz)
{
    (void)args;
    (void)job;
    if (err && errsz)
        err[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}
static mf_step_t stub_probe_step(void *job)
{
    (void)job;
    return MF_STEP_IDLE;
}
static unsigned stub_probe_mask(void *job)
{
    (void)job;
    return 0;
}
static int stub_probe_result(void *job, char *json, size_t cap)
{
    (void)job;
    if (json && cap)
        json[0] = '\0';
    return MF_ERR_UNSUPPORTED;
}
static void stub_probe_close(void *job)
{
    (void)job;
}

static const char *stub_describe(void)
{
    return "{\"fields\":[{\"key\":\"stub.path\",\"label\":\"Stub Path\","
           "\"type\":\"string\",\"required\":true}],"
           "\"capture\":{\"interval_s\":10,\"min_s\":2,\"retention_days\":30,\"graph\":\"ok\","
           "\"columns\":{\"ok\":\"ok\"}}}";
}

static const mf_plugin_ops_t g_ops[2] = {
    {
        .abi = MF_PLUGIN_ABI,
        .ops_size = sizeof(mf_plugin_ops_t),
        .kind = "battery",
        .driver = "stub",
        .version = "0.1.0",
        .open = stub_open,
        .close = stub_close,
        .fd = stub_fd,
        .select_mask = stub_mask,
        .prepare_fds = NULL,
        .step = stub_step,
        .caps = stub_caps,
        .last_error = stub_err,
        .get_reading = stub_reading,
        .get_settings = stub_get_settings,
        .put_settings = stub_put_settings,
        .action = stub_action,
        .probe_start = stub_probe_start,
        .probe_step = stub_probe_step,
        .probe_select_mask = stub_probe_mask,
        .probe_prepare_fds = NULL,
        .probe_result = stub_probe_result,
        .probe_close = stub_probe_close,
        .describe = stub_describe,
    },
    {
        .abi = MF_PLUGIN_ABI,
        .ops_size = sizeof(mf_plugin_ops_t),
        .kind = "charger",
        .driver = "stub",
        .version = "0.1.0",
        .open = stub_open,
        .close = stub_close,
        .fd = stub_fd,
        .select_mask = stub_mask,
        .prepare_fds = NULL,
        .step = stub_step,
        .caps = stub_caps,
        .last_error = stub_err,
        .get_reading = stub_reading,
        .get_settings = stub_get_settings,
        .put_settings = stub_put_settings,
        .action = stub_action,
        .probe_start = stub_probe_start,
        .probe_step = stub_probe_step,
        .probe_select_mask = stub_probe_mask,
        .probe_prepare_fds = NULL,
        .probe_result = stub_probe_result,
        .probe_close = stub_probe_close,
    },
};

#ifdef MF_STUB_OLD_ABI
/* Mimic a plugin built before the optional fields (describe) existed:
 * entries are MF_PLUGIN_OPS_MIN_SIZE bytes, packed back to back. */
static unsigned char g_old[2][MF_PLUGIN_OPS_MIN_SIZE]
    __attribute__((aligned(16)));

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        mf_plugin_ops_t e = g_ops[i];

        e.ops_size = MF_PLUGIN_OPS_MIN_SIZE;
        memcpy(g_old[i], &e, MF_PLUGIN_OPS_MIN_SIZE);
    }
    if (out)
        *out = (const mf_plugin_ops_t *)(const void *)g_old;
    return 2;
}
#else
size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = g_ops;
    return 2;
}
#endif
