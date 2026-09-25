/*
 * The TUI's learned state (see tui_state.h), in a small JSON file:
 *   {"discharge_peaks": {"10.100.0.6:5250": 3508}}
 */

#include "tui_state.h"

#include <cJSON.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static cJSON *g_state;                  /* loaded on first use */

static void state_path(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");

    if (xdg && xdg[0])
        snprintf(out, cap, "%s/moonflare/tui-state.json", xdg);
    else
        snprintf(out, cap, "%s/.local/state/moonflare/tui-state.json",
                 home ? home : ".");
}

static cJSON *state(void)
{
    char path[512];
    FILE *f;

    if (g_state)
        return g_state;
    state_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f)
    {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);

        buf[n] = '\0';
        fclose(f);
        g_state = cJSON_Parse(buf);
    }
    if (!cJSON_IsObject(g_state))
    {
        cJSON_Delete(g_state);
        g_state = cJSON_CreateObject();
    }
    return g_state;
}

/* mkdir -p for the file's directory. */
static void make_dirs(const char *path)
{
    char tmp[512];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    p = strrchr(tmp, '/');
    if (!p)
        return;
    *p = '\0';
    for (p = tmp + 1; *p; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        (void)mkdir(tmp, 0700);
        *p = '/';
    }
    (void)mkdir(tmp, 0700);
}

/* Written whole to a temporary file and renamed over the old one. */
static void save(void)
{
    char path[512], tmp[600];
    char *s = cJSON_PrintUnformatted(state());
    size_t len;
    int fd;

    if (!s)
        return;
    state_path(path, sizeof(path));
    make_dirs(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
    {
        len = strlen(s);
        if (write(fd, s, len) == (ssize_t)len && close(fd) == 0)
            (void)rename(tmp, path);
        else
            (void)unlink(tmp);
    }
    free(s);
}

double mf_state_peak(const char *addr)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(state(), "discharge_peaks"),
        addr ? addr : "");

    return cJSON_IsNumber(v) && v->valuedouble > 0 ? v->valuedouble : 0.0;
}

void mf_state_set_peak(const char *addr, double w)
{
    cJSON *peaks;

    if (!addr || !addr[0])
        return;
    peaks = cJSON_GetObjectItemCaseSensitive(state(), "discharge_peaks");
    if (!cJSON_IsObject(peaks))
    {
        cJSON_DeleteItemFromObjectCaseSensitive(state(), "discharge_peaks");
        peaks = cJSON_AddObjectToObject(state(), "discharge_peaks");
    }
    cJSON_DeleteItemFromObjectCaseSensitive(peaks, addr);
    if (w > 0)
        cJSON_AddNumberToObject(peaks, addr, w);
    save();
}
