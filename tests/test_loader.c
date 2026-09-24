#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

int g_fail;

void mf_log(int prio, const char *fmt, ...)
{
    (void)prio;
    (void)fmt;
}

#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); g_fail++; } while (0)

int main(int argc, char **argv)
{
    const char *dir;
    int old_abi;
    mf_plugin_registry_t reg;
    const mf_plugin_ops_t *batt, *chg;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s plugin-dir\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    old_abi = argc > 2 && strcmp(argv[2], "old") == 0;
    if (mf_plugins_load_dir(&reg, dir) != 0)
        FAIL("load_dir");
    if (reg.nops != 2)
        FAIL("expected 2 ops");
    batt = mf_plugins_find(&reg, "battery", "stub");
    chg = mf_plugins_find(&reg, "charger", "stub");
    if (!batt)
        FAIL("missing battery/stub");
    if (!chg)
        FAIL("missing charger/stub");
    if (batt && batt->abi != MF_PLUGIN_ABI)
        FAIL("battery abi");
    if (chg && chg->fd && chg->fd(NULL) != -1)
        FAIL("silent fd should be -1");
    if (mf_plugins_find(&reg, "battery", "demo") != NULL)
        FAIL("unexpected demo");
    /* A plugin built before describe() existed still loads (both entries,
     * walked by its own ops_size) and its missing field reads NULL. */
    if (old_abi && batt && batt->describe)
        FAIL("old plugin: describe should be NULL");
    if (old_abi && chg && chg->version && strcmp(chg->version, "0.1.0") != 0)
        FAIL("old plugin: second entry misread");
    if (!old_abi && batt && (!batt->describe ||
                             !strstr(batt->describe(), "stub.path")))
        FAIL("new plugin: describe kept");
    mf_plugins_unload(&reg);
    if (g_fail)
        return 1;
    printf("loader: ok\n");
    return 0;
}
