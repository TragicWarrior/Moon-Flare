#include "system.h"

#include <string.h>

double mf_display_soc(double bms_soc, double rem_ah, double full_ah,
                      double avg_cell_v, double current_a)
{
    static const double pt[][2] = {
        {2.80, 0}, {3.00, 5}, {3.20, 10}, {3.25, 20}, {3.28, 30},
        {3.30, 40}, {3.33, 50}, {3.35, 60}, {3.37, 70}, {3.40, 85},
        {3.45, 95}, {3.50, 99}, {3.60, 100}
    };
    double soc = bms_soc;
    int n = (int)(sizeof(pt) / sizeof(pt[0]));
    int i;
    double vsoc;

    if (full_ah > 0 && rem_ah >= 0)
        soc = rem_ah / full_ah * 100.0;
    if (avg_cell_v <= 0 || current_a <= -0.5 || current_a >= 0.5)
        goto clamp;
    if (avg_cell_v <= pt[0][0])
        vsoc = pt[0][1];
    else if (avg_cell_v >= pt[n - 1][0])
        vsoc = pt[n - 1][1];
    else
    {
        vsoc = 100;
        for (i = 1; i < n; i++)
        {
            if (avg_cell_v <= pt[i][0])
            {
                double span = pt[i][0] - pt[i - 1][0];
                double t = span > 0 ? (avg_cell_v - pt[i - 1][0]) / span : 0;
                vsoc = pt[i - 1][1] + t * (pt[i][1] - pt[i - 1][1]);
                break;
            }
        }
    }
    if (soc - vsoc > 20.0 || vsoc - soc > 20.0)
        soc = vsoc;
clamp:
    if (soc < 0)
        soc = 0;
    if (soc > 100)
        soc = 100;
    return soc;
}

void mf_system_init(mf_system_totals_t *t, double input_max_w,
                    double discharge_max_w)
{
    memset(t, 0, sizeof(*t));
    t->input_max_w = input_max_w;
    t->discharge_max_w = discharge_max_w;
    t->soc_pct = -1.0;
}

void mf_system_add_charger(mf_system_totals_t *t, bool active, bool online,
                           double charging_w)
{
    t->chargers_total++;
    if (!active || !online)
        return;
    t->chargers_counted++;
    if (charging_w > 0.0)
        t->input_w += charging_w;
}

void mf_system_add_battery(mf_system_totals_t *t, bool active, bool online,
                           double pack_v, double current_a, double bms_soc,
                           double rem_ah, double full_ah, int cell_count)
{
    double avg_cell_v = cell_count > 0 ? pack_v / cell_count : 0.0;
    double soc, power_w;

    t->batteries_total++;
    if (!active || !online)
        return;
    t->batteries_counted++;

    soc = mf_display_soc(bms_soc, rem_ah, full_ah, avg_cell_v, current_a);
    if (full_ah > 0.0 && pack_v > 0.0)
    {
        double full_wh = full_ah * pack_v;

        t->capacity_wh += full_wh;
        t->stored_wh += full_wh * soc / 100.0;
    }
    else
    {
        t->soc_sum += soc;
        t->soc_n++;
    }

    power_w = pack_v * current_a;
    if (power_w > 0.0)
        t->charge_w += power_w;
    else
        t->discharge_w -= power_w;
}

void mf_system_finish(mf_system_totals_t *t)
{
    if (t->capacity_wh > 0.0)
        t->soc_pct = t->stored_wh / t->capacity_wh * 100.0;
    else if (t->soc_n > 0)
        t->soc_pct = t->soc_sum / t->soc_n;
    else
        t->soc_pct = -1.0;
}
