#include "mf_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double parse_first_cell_v(const char *json)
{
    const char *cells = strstr(json, "\"cells\"");
    const char *p;
    if (!cells)
        return -1.0;
    p = strstr(cells, "\"voltage_v\"");
    if (!p)
        return -1.0;
    p = strchr(p, ':');
    if (!p)
        return -1.0;
    return strtod(p + 1, NULL);
}

static const char *parse_field(const char *json, const char *field)
{
    char key[64];
    snprintf(key, sizeof(key), "\"%s\"", field);
    const char *p = strstr(json, key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '"') p++;
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != '"') q++;
    size_t len = (size_t)(q - p);
    if (len == 0) return NULL;
    static char buf[256];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

static const char *valid_stages[] = {
    "Resting","Absorb","BulkMppt","Float","FloatMppt",
    "Equalize","HyperVoc","EqMppt", NULL
};

int main(void)
{
    const mf_plugin_ops_t *ops = NULL;
    size_t n = mf_plugin_entries(&ops);
    if (n != 2)
    {
        printf("FAIL: expected n=2, got %zu\n", n);
        return 1;
    }
    if (ops[0].abi != MF_PLUGIN_ABI)
    {
        printf("FAIL: battery abi\n");
        return 1;
    }
    if (ops[0].ops_size != sizeof(mf_plugin_ops_t))
    {
        printf("FAIL: battery ops_size\n");
        return 1;
    }
    if (strcmp(ops[0].kind, "battery") != 0 || strcmp(ops[0].driver, "demo") != 0)
    {
        printf("FAIL: battery kind/driver\n");
        return 1;
    }
    if (ops[1].abi != MF_PLUGIN_ABI)
    {
        printf("FAIL: charger abi\n");
        return 1;
    }
    if (ops[1].ops_size != sizeof(mf_plugin_ops_t))
    {
        printf("FAIL: charger ops_size\n");
        return 1;
    }
    if (strcmp(ops[1].kind, "charger") != 0 || strcmp(ops[1].driver, "demo") != 0)
    {
        printf("FAIL: charger kind/driver\n");
        return 1;
    }
    printf("OK: mf_plugin_entries n=%zu\n", n);

    /* Two battery instances — isolation test */
    void *bat_a = ops[0].open("{\"kind\":\"battery\",\"driver\":\"demo\",\"name\":\"A\",\"uuid\":\"aaa\",\"seed\":0}", NULL, 0);
    void *bat_b = ops[0].open("{\"kind\":\"battery\",\"driver\":\"demo\",\"name\":\"B\",\"uuid\":\"bbb\",\"seed\":1}", NULL, 0);
    void *chg   = ops[1].open("{\"kind\":\"charger\",\"driver\":\"demo\",\"name\":\"C\",\"uuid\":\"ccc\"}", NULL, 0);
    if (!bat_a || !bat_b || !chg)
    {
        printf("FAIL: open\n");
        return 1;
    }
    printf("OK: open a=%p b=%p c=%p\n", bat_a, bat_b, chg);

    /* step tick */
    for (int i = 0; i < 5; i++)
    {
        ops[0].step(bat_a);
        ops[0].step(bat_b);
        ops[1].step(chg);
    }
    printf("OK: step() tick-only\n");

    /* fd() */
    if (ops[0].fd(bat_a) != -1)
    {
        printf("FAIL: bat_a fd\n");
        return 1;
    }
    if (ops[0].fd(bat_b) != -1)
    {
        printf("FAIL: bat_b fd\n");
        return 1;
    }
    if (ops[1].fd(chg) != -1)
    {
        printf("FAIL: chg fd\n");
        return 1;
    }
    printf("OK: fd() == -1\n");

    /* select_mask */
    if (ops[0].select_mask(bat_a) != 0)
    {
        printf("FAIL: select_mask\n");
        return 1;
    }
    printf("OK: select_mask == 0\n");

    /* caps */
    unsigned ca = ops[0].caps(bat_a);
    if ((ca & MF_CAP_READ) == 0)
    {
        printf("FAIL: no READ cap\n");
        return 1;
    }
    if ((ca & MF_CAP_ACTION_SWITCH) == 0)
    {
        printf("FAIL: no ACTION_SWITCH cap\n");
        return 1;
    }
    printf("OK: caps READ|ACTION_SWITCH\n");

    /* get_reading battery — check cell 1 seed offset */
    char buf_a[2048], buf_b[2048];
    if (ops[0].get_reading(bat_a, buf_a, sizeof(buf_a)) < 0)
    {
        printf("FAIL: reading a\n");
        return 1;
    }
    if (ops[0].get_reading(bat_b, buf_b, sizeof(buf_b)) < 0)
    {
        printf("FAIL: reading b\n");
        return 1;
    }

    double va = parse_first_cell_v(buf_a);
    double vb = parse_first_cell_v(buf_b);
    if (va < 0 || vb < 0)
    {
        printf("FAIL: parse cell v\n");
        return 1;
    }
    if (fabs(va - vb) < 0.0001)
    {
        printf("FAIL: batteries not isolated (cell1 same)\n");
        return 1;
    }
    printf("OK: battery A cell1=%.4f B cell1=%.4f diff=%.4f\n", va, vb, vb - va);

    double expected = 1.0 * 0.002; /* seed_b=1 - seed_a=0, diff = 1*0.002 */
    if (fabs((vb - va) - expected) > 0.0001)
    {
        printf("FAIL: cell1 diff=%.6f expected ~%.4f\n", vb - va, expected);
        return 1;
    }
    printf("OK: cell1 seed offset = %.4f V (expected %.4f)\n", vb - va, expected);

    /* Charger get_reading — charge_stage */
    char buf_c[512];
    if (ops[1].get_reading(chg, buf_c, sizeof(buf_c)) < 0)
    {
        printf("FAIL: charger reading\n");
        return 1;
    }
    const char *stage = parse_field(buf_c, "charge_stage");
    if (!stage)
    {
        printf("FAIL: no charge_stage\n");
        return 1;
    }
    int found = 0;
    for (int i = 0; valid_stages[i]; i++)
    {
        if (strcmp(stage, valid_stages[i]) == 0)
        {
            found = 1;
            break;
        }
    }
    if (!found)
    {
        printf("FAIL: invalid charge_stage '%s'\n", stage);
        return 1;
    }
    printf("OK: charger charge_stage = '%s'\n", stage);

    /* action set_switch — allowed and isolated */
    char err[128];
    int ret_a = ops[0].action(bat_a, "set_switch", "{\"on\":true}", err, sizeof(err));
    if (ret_a != MF_OK)
    {
        printf("FAIL: bat_a set_switch=%d err='%s'\n", ret_a, err);
        return 1;
    }
    printf("OK: bat_a set_switch=%d\n", ret_a);

    /* Verify battery A's reading did NOT change due to battery B's state */
    char buf_a2[2048];
    ops[0].get_reading(bat_a, buf_a2, sizeof(buf_a2));
    double va2 = parse_first_cell_v(buf_a2);
    if (fabs(va2 - va) > 0.0001)
    {
        printf("FAIL: bat_a changed after action (not isolated)\n");
        return 1;
    }
    printf("OK: bat_a isolated after action\n");

    /* charger action returns unsupported */
    int ret_c = ops[1].action(chg, "set_switch", "{}", err, sizeof(err));
    if (ret_c == MF_OK)
    {
        printf("FAIL: charger should not support set_switch\n");
        return 1;
    }
    printf("OK: charger set_switch=%d (unsupported)\n", ret_c);

    /* close */
    ops[0].close(bat_a);
    ops[0].close(bat_b);
    ops[1].close(chg);
    printf("OK: closed all contexts\n");

    printf("ALL PASS\n");
    return 0;
}
