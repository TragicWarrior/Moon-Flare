#ifndef MF_SYSTEM_H
#define MF_SYSTEM_H

#include <stdbool.h>

/* Display SOC: remaining/full when known; at rest, LFP OCV from mean
 * cell voltage wins if it disagrees with the BMS coulomb counter by >20. */
double mf_display_soc(double bms_soc, double rem_ah, double full_ah,
                      double avg_cell_v, double current_a);

/* System-wide totals over the devices marked active. A device counts only
 * when it is both active and online. Battery power is signed per pack
 * (current_a < 0 = discharging); charge and discharge are summed apart so
 * one pack charging another does not cancel out. */
typedef struct {
    int    chargers_total;
    int    chargers_counted;
    int    batteries_total;
    int    batteries_counted;
    double input_w;
    double input_max_w;
    double capacity_wh;
    double stored_wh;
    double soc_pct;           /* -1 when no counted pack reports SOC */
    double charge_w;
    double discharge_w;
    double discharge_max_w;

    /* SOC is capacity-weighted. Only when no counted pack reports a
     * capacity does it fall back to the plain mean of the packs' SOC. */
    double soc_sum;
    int    soc_n;
} mf_system_totals_t;

void mf_system_init(mf_system_totals_t *t, double input_max_w,
                    double discharge_max_w);
void mf_system_add_charger(mf_system_totals_t *t, bool active, bool online,
                           double charging_w);
/* rem_ah < 0 or full_ah <= 0 means unknown; cell_count <= 0 skips the
 * voltage cross-check. */
void mf_system_add_battery(mf_system_totals_t *t, bool active, bool online,
                           double pack_v, double current_a, double bms_soc,
                           double rem_ah, double full_ah, int cell_count);
void mf_system_finish(mf_system_totals_t *t);

#endif
