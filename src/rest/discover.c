#define _POSIX_C_SOURCE 200809L

#include "discover.h"
#include "device.h"
#include "rest.h"
#include "mf_plugin.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const char *mf_rest_listen_spec(void);

enum { DISC_IDLE = 0, DISC_RUNNING, DISC_DONE, DISC_ERROR };

typedef struct {
    int                     state;
    const mf_plugin_ops_t  *ops;
    void                   *job;
    char                    result[16384];
    char                    err[96];
    double                  t0;
    double                  dummy_until;
} discover_job_t;

static discover_job_t g_disc;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const mf_plugin_ops_t *find_probe(const char *kind, const char *bus)
{
    const mf_plugin_registry_t *reg = mf_devices_registry();
    int i;
    if (!reg)
        return NULL;
    for (i = 0; i < reg->nops; i++)
    {
        const mf_plugin_ops_t *ops = reg->ops[i];
        if (!ops || !ops->probe_start)
            continue;
        if (kind && ops->kind && strcmp(ops->kind, kind) != 0)
            continue;
        if (bus && strcmp(bus, "modbus") == 0 &&
            ops->driver && strcmp(ops->driver, "classic") == 0)
            return ops;
        if (bus && strcmp(bus, "usb") == 0 &&
            ops->driver && strcmp(ops->driver, "xd") == 0)
            return ops;
        if (bus && strcmp(bus, "ble") == 0 &&
            ops->driver && strcmp(ops->driver, "jk") == 0)
            return ops;
        if (!bus)
            return ops;
    }
    return NULL;
}

static void snapshot_result(void)
{
    if (g_disc.ops && g_disc.ops->probe_result && g_disc.job)
    {
        if (g_disc.ops->probe_result(g_disc.job, g_disc.result,
                                     sizeof(g_disc.result)) != 0)
            snprintf(g_disc.result, sizeof(g_disc.result),
                     "{\"status\":\"error\",\"results\":[]}");
        return;
    }
    if (g_disc.state == DISC_RUNNING)
        snprintf(g_disc.result, sizeof(g_disc.result),
                 "{\"status\":\"running\",\"results\":[]}");
    else if (g_disc.state == DISC_ERROR)
        snprintf(g_disc.result, sizeof(g_disc.result),
                 "{\"status\":\"error\",\"results\":[],\"error\":\"%s\"}",
                 g_disc.err[0] ? g_disc.err : "error");
    else
        snprintf(g_disc.result, sizeof(g_disc.result),
                 "{\"status\":\"done\",\"results\":[]}");
}

void mf_discover_close(void)
{
    if (g_disc.ops && g_disc.ops->probe_close && g_disc.job)
        g_disc.ops->probe_close(g_disc.job);
    memset(&g_disc, 0, sizeof(g_disc));
}

int mf_discover_is_running(void)
{
    return g_disc.state == DISC_RUNNING;
}

int mf_discover_start(const char *body, size_t body_len, char *err, size_t errsz)
{
    cJSON *root;
    const char *kind = NULL, *bus = NULL;
    char *args;
    char openerr[96];
    int rc;

    if (err && errsz)
        err[0] = '\0';
    if (g_disc.state == DISC_RUNNING)
    {
        if (err && errsz)
            snprintf(err, errsz, "discover already running");
        return -2;
    }
    mf_discover_close();

    if (body && body_len)
        root = cJSON_ParseWithLength(body, body_len);
    else
        root = cJSON_Parse("{}");
    if (!root)
    {
        if (err && errsz)
            snprintf(err, errsz, "bad request");
        return -1;
    }
    {
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "kind");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "bus");
        if (cJSON_IsString(k))
            kind = k->valuestring;
        if (cJSON_IsString(b))
            bus = b->valuestring;
    }
    if (!kind || !kind[0])
    {
        cJSON_Delete(root);
        if (err && errsz)
            snprintf(err, errsz, "kind required");
        return -1;
    }
    if (!cJSON_GetObjectItemCaseSensitive(root, "auto_net"))
        cJSON_AddBoolToObject(root, "auto_net", 1);
    if (!cJSON_GetObjectItemCaseSensitive(root, "listen"))
    {
        const char *ls = mf_rest_listen_spec();
        if (ls && ls[0])
            cJSON_AddStringToObject(root, "listen", ls);
    }
    args = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!args)
    {
        if (err && errsz)
            snprintf(err, errsz, "oom");
        return -1;
    }

    g_disc.ops = find_probe(kind, bus);
    g_disc.t0 = mono_now();
    g_disc.state = DISC_RUNNING;
    if (g_disc.ops && g_disc.ops->probe_start)
    {
        openerr[0] = '\0';
        rc = g_disc.ops->probe_start(args, &g_disc.job, openerr, sizeof(openerr));
        free(args);
        if (rc != MF_OK)
        {
            snprintf(g_disc.err, sizeof(g_disc.err), "%s",
                     openerr[0] ? openerr : "probe_start failed");
            if (err && errsz)
                snprintf(err, errsz, "%s", g_disc.err);
            g_disc.state = DISC_ERROR;
            snapshot_result();
            return -1;
        }
        snapshot_result();
        return 0;
    }
    free(args);
    /* No probe plugin: hold running briefly so a second POST can 409. */
    g_disc.dummy_until = g_disc.t0 + 2.0;
    snapshot_result();
    return 0;
}

int mf_discover_result(char *json, size_t cap)
{
    if (!json || cap == 0)
        return -1;
    if (g_disc.state == DISC_IDLE)
        snprintf(json, cap, "{\"status\":\"done\",\"results\":[]}");
    else
    {
        snapshot_result();
        snprintf(json, cap, "%s", g_disc.result);
    }
    json[cap - 1] = '\0';
    return 0;
}

void mf_discover_prepare_fds(fd_set *r, fd_set *w, int *maxfd)
{
    if (g_disc.state != DISC_RUNNING || !g_disc.ops || !g_disc.job)
        return;
    if (g_disc.ops->probe_prepare_fds)
        g_disc.ops->probe_prepare_fds(g_disc.job, r, w, maxfd);
}

void mf_discover_step(void)
{
    mf_step_t st;

    if (g_disc.state != DISC_RUNNING)
        return;
    if (g_disc.ops && g_disc.ops->probe_step && g_disc.job)
    {
        st = g_disc.ops->probe_step(g_disc.job);
        if (st == MF_STEP_UPDATED || st == MF_STEP_ERROR)
        {
            g_disc.state = (st == MF_STEP_ERROR) ? DISC_ERROR : DISC_DONE;
            snapshot_result();
        }
        return;
    }
    if (mono_now() >= g_disc.dummy_until)
    {
        g_disc.state = DISC_DONE;
        snapshot_result();
    }
}
