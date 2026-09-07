#ifndef MF_PLUGIN_LOADER_H
#define MF_PLUGIN_LOADER_H

#include "mf_plugin.h"

#include <stddef.h>

#define MF_MAX_PLUGIN_LIBS 16
#define MF_MAX_PLUGIN_OPS  32

typedef struct mf_plugin_lib {
    void  *dl;
    char   path[256];
} mf_plugin_lib_t;

typedef struct mf_plugin_registry {
    mf_plugin_lib_t          libs[MF_MAX_PLUGIN_LIBS];
    int                      nlibs;
    const mf_plugin_ops_t   *ops[MF_MAX_PLUGIN_OPS];
    int                      nops;
} mf_plugin_registry_t;

int  mf_plugins_load_dir(mf_plugin_registry_t *reg, const char *dir);
void mf_plugins_unload(mf_plugin_registry_t *reg);
const mf_plugin_ops_t *mf_plugins_find(const mf_plugin_registry_t *reg,
                                       const char *kind, const char *driver);

#endif
