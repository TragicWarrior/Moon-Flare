/* Phantom averaging: what a phantom reports for the modules it shadows. */

#include "average.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static double num(const cJSON *o, const char *path)
{
    char key[64];
    const char *p = path;
    const cJSON *cur = o;

    while (cur && *p)
    {
        size_t n = strcspn(p, ".");

        snprintf(key, sizeof(key), "%.*s", (int)n, p);
        if (cJSON_IsArray(cur))
            cur = cJSON_GetArrayItem(cur, atoi(key));
        else
            cur = cJSON_GetObjectItemCaseSensitive(cur, key);
        p += n;
        if (*p == '.')
            p++;
    }
    return cJSON_IsNumber(cur) ? cur->valuedouble : NAN;
}

static cJSON *avg_of(const char *a, const char *b, const char *c)
{
    const char *src[3] = { a, b, c };
    cJSON *parsed[3];
    const cJSON *in[3];
    cJSON *out;
    int i, n = 0;

    for (i = 0; i < 3 && src[i]; i++)
    {
        parsed[n] = cJSON_Parse(src[i]);
        in[n] = parsed[n];
        n++;
    }
    out = mf_phantom_average(in, n);
    for (i = 0; i < n; i++)
        cJSON_Delete(parsed[i]);
    return out;
}

static const char *k_xd1 =
    "{\"pack_voltage_v\":53.0,\"current_a\":-10.0,\"power_w\":-530,"
    "\"soc_pct\":80.0,\"cell_count\":16,\"full_capacity_ah\":280,"
    "\"remaining_capacity_ah\":224,\"firmware\":\"V1.2\","
    "\"charge_mosfet_on\":true,"
    "\"cells\":[{\"index\":1,\"voltage_v\":3.30,\"balancing\":false},"
    "{\"index\":2,\"voltage_v\":3.32,\"balancing\":true}],"
    "\"temperatures_c\":[25.0,27.0]}";

static const char *k_xd2 =
    "{\"pack_voltage_v\":54.0,\"current_a\":-12.0,\"power_w\":-648,"
    "\"soc_pct\":90.0,\"cell_count\":16,\"full_capacity_ah\":280,"
    "\"remaining_capacity_ah\":252,\"firmware\":\"V1.3\","
    "\"charge_mosfet_on\":false,"
    "\"cells\":[{\"index\":1,\"voltage_v\":3.34,\"balancing\":true},"
    "{\"index\":2,\"voltage_v\":3.36,\"balancing\":false}],"
    "\"temperatures_c\":[29.0,31.0]}";

static void test_one(void)
{
    cJSON *a = avg_of(k_xd1, NULL, NULL);
    char *s1 = cJSON_PrintUnformatted(a);
    cJSON *orig = cJSON_Parse(k_xd1);

    CHECK(a && cJSON_Compare(a, orig, 1), "one shadow: same reading");
    free(s1);
    cJSON_Delete(orig);
    cJSON_Delete(a);
    CHECK(mf_phantom_average(NULL, 0) == NULL, "no shadows: NULL");
}

static void test_two(void)
{
    cJSON *a = avg_of(k_xd1, k_xd2, NULL);

    CHECK(num(a, "pack_voltage_v") == 53.5, "voltage averaged");
    CHECK(num(a, "current_a") == -11.0, "current averaged");
    CHECK(num(a, "soc_pct") == 85.0, "soc averaged");
    CHECK(num(a, "remaining_capacity_ah") == 238.0, "remaining averaged");
    CHECK(num(a, "cell_count") == 16.0, "equal counts stay");
    CHECK(num(a, "cells.0.voltage_v") == 3.32, "per-cell average");
    CHECK(num(a, "cells.1.voltage_v") == 3.34, "per-cell average 2");
    CHECK(num(a, "cells.1.index") == 2.0, "cell index kept");
    CHECK(num(a, "temperatures_c.0") == 27.0, "per-sensor average");
    CHECK(num(a, "temperatures_c.1") == 29.0, "per-sensor average 2");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(a, "firmware")->valuestring,
                 "V1.2") == 0, "text from the first");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "charge_mosfet_on")),
          "bool from the first");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(
              cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(a, "cells"), 0),
              "balancing")), "nested bool from the first");
    cJSON_Delete(a);
}

/* Units that disagree on shape: missing keys, shorter arrays, nulls. */
static void test_ragged(void)
{
    cJSON *a = avg_of(
        "{\"v\":10,\"t\":[1,2,3],\"x\":null,\"s\":\"a\",\"only\":5}",
        "{\"v\":20,\"t\":[3],\"x\":4}",
        "{\"v\":\"n/a\",\"t\":[5,6],\"x\":6}");

    CHECK(num(a, "v") == 15.0, "non-numbers skipped in a mean");
    CHECK(num(a, "t.0") == 3.0, "arrays by index (all three)");
    CHECK(num(a, "t.1") == 4.0, "arrays by index (two have it)");
    CHECK(num(a, "t.2") == 3.0, "arrays by index (only the first)");
    CHECK(num(a, "x") == 5.0, "a null in the first does not hide numbers");
    CHECK(num(a, "only") == 5.0, "key only the first has");
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(a, "s")->valuestring, "a") == 0,
          "string kept");
    cJSON_Delete(a);
}

static void test_rounding(void)
{
    cJSON *a = avg_of("{\"v\":1}", "{\"v\":2}", "{\"v\":2}");

    CHECK(num(a, "v") == 1.6667, "rounded to 4 decimals");
    cJSON_Delete(a);
}

int main(void)
{
    test_one();
    test_two();
    test_ragged();
    test_rounding();
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_phantom: ok\n");
    return 0;
}
