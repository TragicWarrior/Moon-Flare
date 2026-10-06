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
    mf_system_add_battery(&t, true, true, 53.0, -10.0, 50.0, 100.0, 200.0, 16, 0);
    mf_system_add_battery(&t, false, true, 53.0, -20.0, 90.0, 180.0, 200.0, 16, 0);
    mf_system_finish(&t);
    CHECK(near(t.input_w, 840.0), "only the active online charger counts");
    CHECK(t.chargers_counted == 1 && t.chargers_total == 3, "charger counts");
    CHECK(t.batteries_counted == 1 && t.batteries_total == 2, "battery counts");
    CHECK(near(t.soc_pct, 50.0), "inactive pack left out of SOC");
    CHECK(near(t.discharge_w, 530.0), "inactive pack left out of discharge");
    CHECK(near(t.capacity_wh, 200.0 * 53.0), "capacity from counted pack");

    /* SOC is capacity-weighted, not a plain mean. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 0.0, 100.0, 100.0, 16, 0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 0.0, 0.0, 300.0, 16, 0);
    mf_system_finish(&t);
    CHECK(near(t.soc_pct, 25.0), "SOC weighted by capacity");
    CHECK(near(t.stored_wh, 100.0 * 53.0), "stored Wh");

    /* Charge and discharge are summed apart, never netted. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 50.0, 10.0, 60.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery(&t, true, true, 50.0, -4.0, 40.0, -1.0, 0.0, 0, 0);
    mf_system_finish(&t);
    CHECK(near(t.charge_w, 500.0), "charge summed");
    CHECK(near(t.discharge_w, 200.0), "discharge summed");
    CHECK(near(t.soc_pct, 50.0), "no capacity figures: mean SOC");

    /* Battery limits and inverter ratings: known only when every counted
     * pack (inverter) reports one; inactive ones don't count. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 50.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery_limit(&t, true, true, 200.0 * 53.0);
    mf_system_add_battery(&t, false, true, 53.0, -5.0, 50.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery_limit(&t, false, true, 100.0 * 53.0);
    mf_system_add_inverter(&t, true, true, 4400.0, -1.0, -1.0);
    mf_system_add_inverter(&t, true, true, 4400.0, -1.0, -1.0);
    mf_system_add_inverter(&t, false, true, 4000.0, -1.0, -1.0);
    mf_system_finish(&t);
    CHECK(mf_system_battery_limit_known(&t) && near(t.battery_limit_w, 10600.0),
          "battery limit from the counted pack");
    CHECK(mf_system_inverter_rated_known(&t) && near(t.inverter_rated_w, 8800.0) &&
          t.inverters_counted == 2 && t.inverters_total == 3,
          "inverter capacity from the counted inverters");
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 50.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery_limit(&t, true, true, 200.0 * 53.0);
    mf_system_add_battery(&t, true, true, 53.0, -5.0, 50.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery_limit(&t, true, true, 0.0);   /* doesn't say */
    mf_system_add_inverter(&t, true, true, 0.0, -1.0, -1.0);
    mf_system_finish(&t);
    CHECK(!mf_system_battery_limit_known(&t), "one pack without a limit: unknown");
    CHECK(!mf_system_inverter_rated_known(&t), "an unrated inverter: unknown");
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_finish(&t);
    CHECK(!mf_system_battery_limit_known(&t) && !mf_system_inverter_rated_known(&t),
          "nothing counted: unknown");

    /* Discharge: what leaves the bank, the counted packs' discharge,
     * whatever the inverters say they draw. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 52.0, -10.0, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery(&t, false, true, 52.0, -8.0, 80.0, -1.0, 0.0, 0, 0);  /* inactive */
    mf_system_add_inverter(&t, true, true, 4400.0, 1205.2, 1080.0);
    mf_system_finish(&t);
    CHECK(near(t.discharge_w, 520.0) && near(t.battery_discharge_w, 520.0),
          "discharge: the counted packs', with an inverter reporting");
    CHECK(mf_system_inverter_output_known(&t) && near(t.inverter_output_w, 1205.2),
          "discharge: the inverters' DC draw is still summed, not used");

    /* Load: the counted inverters' AC output when every one reports it (a
     * phantom counts like any inverter).  Read at batteryman, inverting:
     * 120 V x 9 A and 121 V x 9 A, with 1593 W of solar and the packs
     * giving 1082 W. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_charger(&t, true, true, 1593.0);
    mf_system_add_battery(&t, true, true, 53.04, -10.2, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_battery(&t, true, true, 53.04, -10.2, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_inverter(&t, true, true, 4400.0, 1214.4, 1080.0);  /* the master */
    mf_system_add_inverter(&t, true, true, 4400.0, 1264.8, 1089.0);  /* the slave */
    mf_system_add_inverter(&t, false, true, 4400.0, 900.0, 800.0);   /* inactive */
    mf_system_finish(&t);
    CHECK(t.load_from_inverters && near(t.load_w, 2169.0),
          "load: the counted inverters' AC output");
    CHECK(near(t.load_net_w, 1593.0 + 2.0 * 53.04 * 10.2),
          "load: input plus the packs' discharge, kept beside it");
    CHECK(near(t.discharge_w, 2.0 * 53.04 * 10.2), "load: discharge is still the packs'");

    /* On the grid at night: the inverters pass 18 A through and charge the
     * bank.  The load is what they hand the site; nothing leaves the DC
     * side. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_battery(&t, true, true, 53.5, 38.0, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_inverter(&t, true, true, 4400.0, 0.0, 121.0 * 18.0);
    mf_system_add_inverter(&t, true, true, 4400.0, 0.0, 0.0);
    mf_system_finish(&t);
    CHECK(t.load_from_inverters && near(t.load_w, 2178.0),
          "load: grid power passed through counts");
    CHECK(near(t.load_net_w, 0.0) && near(t.discharge_w, 0.0),
          "load: charging from the grid, the DC side hands over nothing");

    /* Without every inverter's AC output: the DC side.  Solar covering the
     * load and charging the bank is input less charge. */
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_charger(&t, true, true, 3000.0);
    mf_system_add_battery(&t, true, true, 50.0, 20.0, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_inverter(&t, true, true, 4400.0, 1205.2, 1080.0);
    mf_system_add_inverter(&t, true, true, 4400.0, -1.0, -1.0);      /* doesn't say */
    mf_system_finish(&t);
    CHECK(!t.load_from_inverters && near(t.load_w, 2000.0) && near(t.load_net_w, 2000.0),
          "load: an inverter without AC output: input less charge");
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_charger(&t, true, true, 500.0);
    mf_system_add_battery(&t, true, true, 52.0, -10.0, 80.0, -1.0, 0.0, 0, 0);
    mf_system_add_inverter(&t, true, false, 4400.0, 1205.2, 1080.0); /* offline */
    mf_system_finish(&t);
    CHECK(!t.load_from_inverters && near(t.load_w, 1020.0),
          "load: no counted inverter: input plus discharge");
    mf_system_init(&t, 3500.0, 3000.0);
    mf_system_add_charger(&t, true, true, 100.0);
    mf_system_add_battery(&t, true, true, 50.0, 10.0, 80.0, -1.0, 0.0, 0, 0);
    mf_system_finish(&t);
    CHECK(near(t.load_net_w, 0.0) && near(t.load_w, 0.0), "load: never below nothing");

    /* The voltage check (JK only, for now): at rest, a counter >20 points
     * off the LFP curve loses; under load the BMS stands. */
    CHECK(near(mf_display_soc(90.0, -1.0, 0.0, 3.30, 0.0, 1), 40.0),
          "display SOC: with the check, voltage wins at rest");
    CHECK(near(mf_display_soc(90.0, -1.0, 0.0, 3.30, -5.0, 1), 90.0),
          "display SOC: with the check, BMS kept under load");
    CHECK(near(mf_display_soc(99.0, -1.0, 0.0, 3.3475, 0.0, 1), 58.75),
          "display SOC: the JK at 3.35 V/cell reads its voltage");
    /* Without it the BMS always stands (an XD at 82.67% and 3.33 V/cell
     * read 49% whenever it reported 0.00 A). */
    CHECK(near(mf_display_soc(0.0, 165.34, 200.0, 53.24 / 16.0, 0.0, 0), 82.67),
          "display SOC: no check, BMS kept at rest");
    CHECK(mf_soc_voltage_check("jk") && !mf_soc_voltage_check("xd") &&
          !mf_soc_voltage_check("phantom") && !mf_soc_voltage_check(NULL),
          "only JK packs get the check");

    if (g_fail)
        return 1;
    printf("test_system: ok\n");
    return 0;
}
