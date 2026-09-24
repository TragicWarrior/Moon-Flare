#include "system/system.h"

#include <math.h>
#include <stdio.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static int near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

int main(void)
{
    mf_system_totals_t t;

    /* Nothing configured: SOC unknown, all totals zero. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_finish(&t);
    CHECK(t.soc_pct < 0.0, "empty: soc unknown");
    CHECK(t.input_w == 0.0 && t.discharge_w == 0.0, "empty: zero power");
    CHECK(near(t.input_max_w, 3500.0) && near(t.discharge_max_w, 3000.0),
          "empty: full-scale carried through");

    /* Inactive and offline devices are listed but never counted. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_charger(&t, true, true, 840.0);
    mf_system_add_charger(&t, false, true, 500.0);
    mf_system_add_charger(&t, true, false, 300.0);
    mf_system_add_battery(&t, true, true, 53.0, -10.0, 50.0, 100.0, 200.0, 16);
    mf_system_add_battery(&t, false, true, 53.0, -20.0, 90.0, 180.0, 200.0, 16);
    mf_system_finish(&t);
    CHECK(near(t.input_w, 840.0), "only the active online charger counts");
    CHECK(t.chargers_counted == 1 && t.chargers_total == 3, "charger counts");
    CHECK(t.batteries_counted == 1 && t.batteries_total == 2, "battery counts");
    CHECK(near(t.soc_pct, 50.0), "inactive pack left out of SOC");
    CHECK(near(t.discharge_w, 530.0), "inactive pack left out of discharge");
    CHECK(near(t.capacity_wh, 200.0 * 53.0), "capacity from counted pack");

    /* SOC is capacity-weighted, not a plain mean. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 0.0, 100.0, 100.0, 16);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 0.0, 0.0, 300.0, 16);
    mf_system_finish(&t);
    CHECK(near(t.soc_pct, 25.0), "SOC weighted by capacity");
    CHECK(near(t.stored_wh, 100.0 * 53.0), "stored Wh");

    /* Charge and discharge are summed apart, never netted. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 50.0, 10.0, 60.0, -1.0, 0.0, 0);
    mf_system_add_battery(&t, true, true, 50.0, -4.0, 40.0, -1.0, 0.0, 0);
    mf_system_finish(&t);
    CHECK(near(t.charge_w, 500.0), "charge summed");
    CHECK(near(t.discharge_w, 200.0), "discharge summed");
    CHECK(near(t.soc_pct, 50.0), "no capacity figures: mean SOC");

    /* At rest, a coulomb counter >20 points off the LFP voltage curve loses. */
    CHECK(near(mf_display_soc(90.0, -1.0, 0.0, 3.30, 0.0), 40.0),
          "display SOC: voltage wins at rest");
    CHECK(near(mf_display_soc(90.0, -1.0, 0.0, 3.30, -5.0), 90.0),
          "display SOC: BMS kept under load");

    if (g_fail)
        return 1;
    printf("test_system: ok\n");
    return 0;
}
