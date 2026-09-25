/*
 * dlopen loader for libmf_*.so. Enumerate the plugin dir at startup
 * even if no device of that driver exists (GET /api/v1/drivers).
 * Does not waitpid (helpers are reaped by main). Refuses abi mismatch.
 */

#include "loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

void mf_log(int prio, const char *fmt, ...);

#define LOG_W(...) mf_log(LOG_WARNING, __VA_ARGS__)
#define LOG_I(...) mf_log(LOG_INFO,    __VA_ARGS__)

static int ends_with(const char *s, const char *suf)
{
    size_t n = strlen(s);
    size_t m = strlen(suf);
    if (n < m)
        return 0;
    return strcmp(s + (n - m), suf) == 0;
}

int mf_plugins_load_dir(mf_plugin_registry_t *reg, const char *dir)
{
    DIR *d;
    struct dirent *de;

    memset(reg, 0, sizeof(*reg));
    if (!dir || !dir[0])
        return 0;

    d = opendir(dir);
    if (!d)
    {
        LOG_W("plugin dir %s: %s", dir, strerror(errno));
        return -1;
    }

    while ((de = readdir(d)) != NULL)
    {
        char path[256];
        void *dl;
        size_t (*entries)(const mf_plugin_ops_t **);
        const mf_plugin_ops_t *ops = NULL;
        size_t n = 0, i, stride;
        int nwr;

        if (de->d_name[0] == '.')
            continue;
        if (strncmp(de->d_name, "libmf_", 6) != 0)
            continue;
        if (!ends_with(de->d_name, ".so"))
            continue;
        if (reg->nlibs >= MF_MAX_PLUGIN_LIBS)
            break;

        nwr = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (nwr < 0 || (size_t)nwr >= sizeof(path))
            continue;

        dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!dl)
        {
            LOG_W("dlopen %s: %s", path, dlerror());
            continue;
        }
        entries = (size_t (*)(const mf_plugin_ops_t **))dlsym(dl, "mf_plugin_entries");
        if (!entries)
        {
            LOG_W("%s: missing mf_plugin_entries", path);
            dlclose(dl);
            continue;
        }
        n = entries(&ops);
        if (n == 0 || ops == NULL)
        {
            LOG_W("%s: mf_plugin_entries returned empty", path);
            dlclose(dl);
            continue;
        }
        /* Walk the table by the plugin's own entry size: an older plugin's
           entries are shorter than today's mf_plugin_ops_t. */
        stride = ops[0].ops_size;
        for (i = 0; i < n; i++)
        {
            const mf_plugin_ops_t *e =
                (const mf_plugin_ops_t *)((const char *)ops + i * stride);

            if (e->abi != MF_PLUGIN_ABI ||
                e->ops_size < MF_PLUGIN_OPS_MIN_SIZE ||
                e->ops_size != stride)
            {
                LOG_W("%s: refuse abi=%u ops_size=%u", path,
                      (unsigned)e->abi, (unsigned)e->ops_size);
                n = 0;
                break;
            }
        }
        if (n == 0)
        {
            dlclose(dl);
            continue;
        }
        if (reg->nops + (int)n > MF_MAX_PLUGIN_OPS)
        {
            LOG_W("%s: ops table full", path);
            dlclose(dl);
            continue;
        }
        {
            mf_plugin_lib_t *lib = &reg->libs[reg->nlibs++];
            lib->dl = dl;
            snprintf(lib->path, sizeof(lib->path), "%s", path);
        }
        for (i = 0; i < n; i++)
        {
            mf_plugin_ops_t *slot = &reg->store[reg->nops];
            const char *src = (const char *)ops + i * stride;

            memset(slot, 0, sizeof(*slot));
            memcpy(slot, src, stride < sizeof(*slot) ? stride : sizeof(*slot));
            reg->ops[reg->nops++] = slot;
            LOG_I("plugin %s kind=%s driver=%s ver=%s", path,
                  slot->kind ? slot->kind : "?",
                  slot->driver ? slot->driver : "?",
                  slot->version ? slot->version : "?");
        }
    }
    closedir(d);
    return 0;
}

void mf_plugins_unload(mf_plugin_registry_t *reg)
{
    int i;
    for (i = 0; i < reg->nlibs; i++)
    {
        if (reg->libs[i].dl)
            dlclose(reg->libs[i].dl);
        reg->libs[i].dl = NULL;
    }
    reg->nlibs = 0;
    reg->nops = 0;
}

const mf_plugin_ops_t *mf_plugins_find(const mf_plugin_registry_t *reg,
                                       const char *kind, const char *driver)
{
    int i;
    if (!reg)
        return NULL;
    for (i = 0; i < reg->nops; i++)
    {
        const mf_plugin_ops_t *o = reg->ops[i];
        if (kind && o->kind && strcmp(o->kind, kind) != 0)
            continue;
        if (driver && o->driver && strcmp(o->driver, driver) != 0)
            continue;
        return o;
    }
    return NULL;
}

int mf_plugins_add_builtin(mf_plugin_registry_t *reg, const mf_plugin_ops_t *ops)
{
    mf_plugin_ops_t *slot;

    if (!reg || !ops || !ops->kind || !ops->driver)
        return -1;
    if (reg->nops >= MF_MAX_PLUGIN_OPS)
    {
        LOG_W("builtin %s/%s: ops table full", ops->kind, ops->driver);
        return -1;
    }
    if (mf_plugins_find(reg, ops->kind, ops->driver))
    {
        LOG_W("builtin %s/%s: a plugin already provides it", ops->kind,
              ops->driver);
        return -1;
    }
    slot = &reg->store[reg->nops];
    memcpy(slot, ops, sizeof(*slot));
    reg->ops[reg->nops++] = slot;
    LOG_I("builtin kind=%s driver=%s ver=%s", ops->kind, ops->driver,
          ops->version ? ops->version : "?");
    return 0;
}
