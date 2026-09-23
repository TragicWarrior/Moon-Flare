#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cli_render.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>

/* ── --list devices ──────────────────────────────────────────────────── */

int cli_render_list_devices(const cJSON *devices, int raw)
{
    cJSON *item;

    if (raw)
    {
        char *s = cJSON_PrintUnformatted(devices);
        if (s)
        {
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }

    if (!devices || !cJSON_IsArray(devices))
        return -1;

    printf("%-38s %-10s %-8s %-10s %s\n", "ID", "NAME", "KIND", "DRIVER", "ONLINE");

    cJSON_ArrayForEach(item, devices)
    {
        const cJSON *id, *name, *kind, *driver, *online;
        id = cJSON_GetObjectItemCaseSensitive(item, "id");
        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        driver = cJSON_GetObjectItemCaseSensitive(item, "driver");
        online = cJSON_GetObjectItemCaseSensitive(item, "online");
        if (!cJSON_IsString(id) || !cJSON_IsString(name) ||
            !cJSON_IsString(kind) || !cJSON_IsString(driver))
            continue;
        if (!cJSON_IsBool(online))
            continue;
        printf("%-38s %-10s %-8s %-10s %s\n",
            id->valuestring, name->valuestring,
            kind->valuestring, driver->valuestring,
            online->valueint ? "yes" : "no");
    }

    return 0;
}

/* ── --list drivers ─────────────────────────────────────────────────── */

int cli_render_list_drivers(const cJSON *drivers, int raw)
{
    cJSON *item, *cap;
    cJSON *caps;

    if (raw)
    {
        char *s = cJSON_PrintUnformatted(drivers);
        if (s)
        {
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }

    if (!drivers || !cJSON_IsArray(drivers))
        return -1;

    printf("%-20s %-20s %s\n", "KIND", "DRIVER", "CAPABILITIES");

    cJSON_ArrayForEach(item, drivers)
    {
        const cJSON *kind, *driver;
        char capbuf[2048];
        memset(capbuf, 0, sizeof(capbuf));
        kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        driver = cJSON_GetObjectItemCaseSensitive(item, "driver");
        caps = cJSON_GetObjectItemCaseSensitive(item, "caps");
        if (!cJSON_IsString(kind) || !cJSON_IsString(driver))
            continue;
        if (cJSON_IsArray(caps))
        {
            int first = 1;
            cJSON_ArrayForEach(cap, caps)
            {
                if (!cJSON_IsString(cap))
                    continue;
                if (!first)
                {
                    size_t len = strlen(capbuf);
                    if (len + 3 < sizeof(capbuf))
                        strcat(capbuf, ", ");
                }
                {
                    size_t clen = strlen(capbuf);
                    size_t addlen = strlen(cap->valuestring);
                    if (clen + addlen < sizeof(capbuf))
                        strcat(capbuf, cap->valuestring);
                }
                first = 0;
            }
        }
        printf("%-20s %-20s %s\n", kind->valuestring, driver->valuestring, capbuf);
    }

    return 0;
}

/* ── --query ─────────────────────────────────────────────────────────── */

int cli_render_query_device(const cJSON *dev, int raw)
{
    char *s;
    const cJSON *name, *kind, *driver, *state, *data, *online;

    if (raw)
    {
        s = cJSON_Print(dev);
        if (s)
        {
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }

    if (!dev || !cJSON_IsObject(dev))
        return -1;

    name = cJSON_GetObjectItemCaseSensitive(dev, "name");
    kind = cJSON_GetObjectItemCaseSensitive(dev, "kind");
    driver = cJSON_GetObjectItemCaseSensitive(dev, "driver");
    state = cJSON_GetObjectItemCaseSensitive(dev, "state");
    data = cJSON_GetObjectItemCaseSensitive(dev, "data");
    online = cJSON_GetObjectItemCaseSensitive(dev, "online");

    if (!cJSON_IsString(name) || !cJSON_IsString(kind) ||
        !cJSON_IsString(driver))
        return -1;
    if (!cJSON_IsBool(online))
        return -1;

    {
        const char *state_str = "unknown";
        if (state && cJSON_IsString(state) && state->valuestring)
            state_str = state->valuestring;
        printf("%s  (%s / %s)                     online \xc2\xb7 %s\n",
            name->valuestring, kind->valuestring, driver->valuestring,
            online->valueint ? state_str : "offline");
    }

    if (strcmp(kind->valuestring, "battery") == 0 && cJSON_IsObject(data))
    {
        {
            const cJSON *pv, *cv, *soc, *soh;
            pv = cJSON_GetObjectItemCaseSensitive(data, "pack_voltage_v");
            cv = cJSON_GetObjectItemCaseSensitive(data, "current_a");
            soc = cJSON_GetObjectItemCaseSensitive(data, "soc_pct");
            soh = cJSON_GetObjectItemCaseSensitive(data, "soh_pct");
            printf("  Pack");
            if (cJSON_IsNumber(pv))
                printf("   %.2f V", pv->valuedouble);
            if (cJSON_IsNumber(cv))
                printf("   %.2f A", cv->valuedouble);
            if (cJSON_IsNumber(soc))
                printf("   SOC %.1f%%", soc->valuedouble);
            if (cJSON_IsNumber(soh))
                printf("   SOH %.1f%%", soh->valuedouble);
            printf("\n");
        }
        {
            const cJSON *full, *rem;
            full = cJSON_GetObjectItemCaseSensitive(data, "full_capacity_ah");
            rem = cJSON_GetObjectItemCaseSensitive(data, "remaining_capacity_ah");
            if (cJSON_IsNumber(full) || cJSON_IsNumber(rem))
            {
                printf("  Capacity");
                if (cJSON_IsNumber(rem))
                    printf("   %.1f", rem->valuedouble);
                if (cJSON_IsNumber(full))
                    printf(" / %.1f Ah", full->valuedouble);
                printf(" remaining\n");
            }
        }
        {
            const cJSON *cmos, *dmos, *bswitch;
            cmos = cJSON_GetObjectItemCaseSensitive(data, "charge_mosfet_on");
            dmos = cJSON_GetObjectItemCaseSensitive(data, "discharge_mosfet_on");
            bswitch = cJSON_GetObjectItemCaseSensitive(data, "balancer_switch");
            if (cJSON_IsBool(cmos) || cJSON_IsBool(dmos) || cJSON_IsBool(bswitch))
            {
                printf("  MOSFET");
                if (cJSON_IsBool(cmos))
                    printf("   %s", cmos->valueint ? "charge on" : "charge off");
                if (cJSON_IsBool(dmos))
                    printf("   %s", dmos->valueint ? "discharge on" : "discharge off");
                if (cJSON_IsBool(bswitch))
                    printf("   %s", bswitch->valueint ? "balancer on" : "balancer off");
                printf("\n");
            }
        }
        {
            const cJSON *temps, *labels;
            int n, i;
            temps = cJSON_GetObjectItemCaseSensitive(data, "temperatures_c");
            labels = cJSON_GetObjectItemCaseSensitive(data, "temp_labels");
            if (cJSON_IsArray(temps) && cJSON_IsArray(labels))
            {
                n = cJSON_GetArraySize(temps);
                if (n > 0)
                {
                    int first = 1;
                    printf("  Temps  ");
                    for (i = 0; i < n; i++)
                    {
                        cJSON *t = cJSON_GetArrayItem(temps, i);
                        const cJSON *lb = cJSON_GetArrayItem(labels, i);
                        if (!cJSON_IsNumber(t))
                            continue;
                        if (!first)
                            printf("   ");
                        if (lb && cJSON_IsString(lb))
                            printf("%s %.1f\u00b0C", lb->valuestring, t->valuedouble);
                        else
                            printf("T%d %.1f\u00b0C", i, t->valuedouble);
                        first = 0;
                    }
                    printf("\n");
                }
            }
        }
        {
            const cJSON *cells;
            int n, i;
            double min_v = 999.0, max_v = -1.0;
            int min_idx = 0, max_idx = 0;
            int cell_count = 0;
            cells = cJSON_GetObjectItemCaseSensitive(data, "cells");
            if (cJSON_IsArray(cells))
            {
                n = cJSON_GetArraySize(cells);
                for (i = 0; i < n; i++)
                {
                    cJSON *cell = cJSON_GetArrayItem(cells, i);
                    const cJSON *cv, *ci;
                    double v;
                    int idx;
                    if (!cJSON_IsObject(cell))
                        continue;
                    ci = cJSON_GetObjectItemCaseSensitive(cell, "index");
                    cv = cJSON_GetObjectItemCaseSensitive(cell, "voltage_v");
                    if (!cJSON_IsNumber(ci) || !cJSON_IsNumber(cv))
                        continue;
                    idx = (int)ci->valuedouble;
                    v = cv->valuedouble;
                    if (v < min_v)
                    {
                        min_v = v;
                        min_idx = idx;
                    }
                    if (v > max_v)
                    {
                        max_v = v;
                        max_idx = idx;
                    }
                    cell_count++;
                }
                if (cell_count > 0)
                {
                    double delta_mv = (max_v - min_v) * 1000.0;
                    printf("  Cells (%d)  min %.3f (#%d)   max %.3f (#%d)   \xc2\x94 %.0f mV\n",
                        cell_count, min_v, min_idx, max_v, max_idx, delta_mv);
                    {
                        int line = 0;
                        for (i = 0; i < n; i++)
                        {
                            cJSON *cell = cJSON_GetArrayItem(cells, i);
                            const cJSON *cv2, *ci2;
                            double v;
                            int idx;
                            if (!cJSON_IsObject(cell))
                                continue;
                            ci2 = cJSON_GetObjectItemCaseSensitive(cell, "index");
                            cv2 = cJSON_GetObjectItemCaseSensitive(cell, "voltage_v");
                            if (!cJSON_IsNumber(ci2) || !cJSON_IsNumber(cv2))
                                continue;
                            idx = (int)ci2->valuedouble;
                            v = cv2->valuedouble;
                            if (line % 8 == 0)
                                printf("  ");
                            printf("%d: %.3f  ", idx, v);
                            line++;
                        }
                        printf("\n");
                    }
                }
            }
        }
    }
    else if (strcmp(kind->valuestring, "charger") == 0 && cJSON_IsObject(data))
    {
        const cJSON *bv, *cw, *ks, *st;
        bv = cJSON_GetObjectItemCaseSensitive(data, "battery_voltage_v");
        cw = cJSON_GetObjectItemCaseSensitive(data, "charging_watts");
        ks = cJSON_GetObjectItemCaseSensitive(data, "kwh_today");
        st = cJSON_GetObjectItemCaseSensitive(data, "charge_stage");
        printf("  Charger");
        if (cJSON_IsNumber(bv))
            printf("   %.1f V", bv->valuedouble);
        if (cJSON_IsNumber(cw))
            printf("   %d W", (int)cw->valuedouble);
        if (cJSON_IsNumber(ks))
            printf("   %.1f kWh", ks->valuedouble);
        if (cJSON_IsString(st) && st->valuestring)
            printf("   stage: %s", st->valuestring);
        printf("\n");
    }
    else
    {
        printf("  (%s device)\n", kind->valuestring);
    }

    return 0;
}

/* ── --status ────────────────────────────────────────────────────────── */

static void print_device_line(const char *prefix, const cJSON *item)
{
    const cJSON *name, *driver, *id, *online;
    name = cJSON_GetObjectItemCaseSensitive(item, "name");
    driver = cJSON_GetObjectItemCaseSensitive(item, "driver");
    id = cJSON_GetObjectItemCaseSensitive(item, "id");
    online = cJSON_GetObjectItemCaseSensitive(item, "online");
    printf("%s  %-12s %-8s %-12s  %s", prefix,
        cJSON_IsString(name) ? name->valuestring : "?",
        cJSON_IsString(driver) ? driver->valuestring : "?",
        cJSON_IsString(id) ? id->valuestring : "?",
        cJSON_IsBool(online) ? (online->valueint ? "online" : "offline") : "?");
}

int cli_render_status(const cJSON *status, int raw)
{
    const cJSON *batteries, *chargers, *inverters, *phantoms;
    cJSON *item;
    const cJSON *server;

    if (raw)
    {
        char *s = cJSON_PrintUnformatted(status);
        if (s)
        {
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }

    if (!status || !cJSON_IsObject(status))
        return -1;

    server = cJSON_GetObjectItemCaseSensitive(status, "server");
    if (cJSON_IsString(server) && server->valuestring)
        printf("%s\n", server->valuestring);

    batteries = cJSON_GetObjectItemCaseSensitive(status, "batteries");
    if (cJSON_IsArray(batteries) && cJSON_GetArraySize(batteries) > 0)
    {
        const cJSON *pv, *cv, *soc;
        printf("\nBatteries:\n");
        cJSON_ArrayForEach(item, batteries)
        {
            print_device_line("", item);
            pv = cJSON_GetObjectItemCaseSensitive(item, "pack_voltage_v");
            cv = cJSON_GetObjectItemCaseSensitive(item, "current_a");
            soc = cJSON_GetObjectItemCaseSensitive(item, "soc_pct");
            if (cJSON_IsNumber(pv))
                printf("   %.2f V", pv->valuedouble);
            if (cJSON_IsNumber(cv))
                printf("   %.1f A", cv->valuedouble);
            if (cJSON_IsNumber(soc))
                printf("   SOC %.1f%%", soc->valuedouble);
            printf("\n");
        }
    }

    chargers = cJSON_GetObjectItemCaseSensitive(status, "chargers");
    if (cJSON_IsArray(chargers) && cJSON_GetArraySize(chargers) > 0)
    {
        const cJSON *bv, *cw, *ks, *st;
        printf("\nChargers:\n");
        cJSON_ArrayForEach(item, chargers)
        {
            print_device_line("", item);
            bv = cJSON_GetObjectItemCaseSensitive(item, "battery_voltage_v");
            cw = cJSON_GetObjectItemCaseSensitive(item, "charging_watts");
            ks = cJSON_GetObjectItemCaseSensitive(item, "kwh_today");
            st = cJSON_GetObjectItemCaseSensitive(item, "charge_stage");
            if (cJSON_IsNumber(bv))
                printf("   %.1f V", bv->valuedouble);
            if (cJSON_IsNumber(cw))
                printf("   %d W", (int)cw->valuedouble);
            if (cJSON_IsNumber(ks))
                printf("   %.1f kWh", ks->valuedouble);
            if (cJSON_IsString(st) && st->valuestring)
                printf("   %s", st->valuestring);
            printf("\n");
        }
    }

    inverters = cJSON_GetObjectItemCaseSensitive(status, "inverters");
    if (cJSON_IsArray(inverters) && cJSON_GetArraySize(inverters) > 0)
    {
        printf("\nInverters:\n");
        cJSON_ArrayForEach(item, inverters)
        {
            print_device_line("", item);
            printf("\n");
        }
    }

    phantoms = cJSON_GetObjectItemCaseSensitive(status, "phantoms");
    if (cJSON_IsArray(phantoms) && cJSON_GetArraySize(phantoms) > 0)
    {
        printf("\nPhantoms:\n");
        cJSON_ArrayForEach(item, phantoms)
        {
            print_device_line("", item);
            printf("\n");
        }
    }

    return 0;
}

/* ── --history ───────────────────────────────────────────────────────── */

int cli_render_history(const cJSON *history, int raw)
{
    char *s;
    const cJSON *ts, *values, *column;
    int n, i;

    if (raw)
    {
        s = cJSON_PrintUnformatted(history);
        if (s)
        {
            printf("%s\n", s);
            free(s);
        }
        return 0;
    }

    if (!history || !cJSON_IsObject(history))
        return -1;

    column = cJSON_GetObjectItemCaseSensitive(history, "column");
    ts = cJSON_GetObjectItemCaseSensitive(history, "ts");
    values = cJSON_GetObjectItemCaseSensitive(history, "values");

    if (!cJSON_IsArray(ts) || !cJSON_IsArray(values))
        return -1;

    n = cJSON_GetArraySize(ts);
    if (n == 0)
        return 0;

    if (cJSON_IsString(column) && column->valuestring)
        printf("History (%s): %d points\n", column->valuestring, n);
    else
        printf("History: %d points\n", n);

    {
        int start = n > 20 ? n - 20 : 0;
        for (i = start; i < n; i++)
        {
            cJSON *tv = cJSON_GetArrayItem(ts, i);
            cJSON *vv = cJSON_GetArrayItem(values, i);
            if (!cJSON_IsNumber(tv) || !cJSON_IsNumber(vv))
                continue;
            {
                time_t t = (time_t)tv->valuedouble;
                struct tm *tm_info;
                char timebuf[32];
                tm_info = localtime(&t);
                strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
                printf("  %-8s  %s  %s\n", timebuf,
                    cJSON_IsString(column) && column->valuestring ? column->valuestring : "value",
                    vv->valuestring);
            }
        }
    }

    {
        int spark_len = 40;
        double min_val = 999999, max_val = -999999;
        int j;
        int *bars;
        for (j = 0; j < n; j++)
        {
            cJSON *vv = cJSON_GetArrayItem(values, j);
            if (cJSON_IsNumber(vv))
            {
                if (vv->valuedouble < min_val)
                    min_val = vv->valuedouble;
                if (vv->valuedouble > max_val)
                    max_val = vv->valuedouble;
            }
        }
        bars = (int *)calloc((size_t)spark_len, sizeof(int));
        if (bars)
        {
            double range = max_val - min_val;
            if (range == 0)
                range = 1.0;
            for (j = 0; j < n; j++)
            {
                cJSON *vv = cJSON_GetArrayItem(values, j);
                int pos;
                if (!cJSON_IsNumber(vv))
                    continue;
                pos = (int)((vv->valuedouble - min_val) / range * (double)(spark_len - 1));
                if (pos < 0)
                    pos = 0;
                if (pos >= spark_len)
                    pos = spark_len - 1;
                bars[pos]++;
            }
            printf("  [");
            for (j = 0; j < spark_len; j++)
            {
                if (bars[j] > 0)
                    printf("#");
                else
                    printf(" ");
            }
            printf("]\n");
            free(bars);
        }
    }

    return 0;
}

/* ── Usage ───────────────────────────────────────────────────────────── */

void cli_print_usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [GLOBAL OPTS] <command>\n"
        "\n"
        "Commands:\n"
        "  -l, --list [WHAT]       list devices (WHAT defaults to \"devices\";\n"
        "                          also \"drivers\")\n"
        "  -q, --query <ID>        full readout for one device\n"
        "      --status            one-shot summary of all devices\n"
        "      --mcp               run as an MCP server on stdio\n"
        "  -h, --help              usage\n"
        "      --version           print version and exit\n"
        "\n"
        "Global options:\n"
        "      --connect HOST:PORT daemon REST address\n"
        "      --config PATH       path to moonflare.json / moonflared.json\n"
        "  -f, --formatted         human-friendly ASCII report (default)\n"
        "  -r, --raw               emit the daemon's raw JSON response\n"
        "      --timeout SECONDS   per-request timeout (default 5)\n",
        progname ? progname : "moonflare-cli");
}
