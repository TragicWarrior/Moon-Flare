#ifndef MF_SYSTEM_H
#define MF_SYSTEM_H

#include <stdbool.h>

/* Display SOC: remaining/full when known, else the BMS's SOC.  With
 * voltage_check, at rest (under 0.5 A) the LFP voltage curve wins when it
 * disagrees with the BMS by >20 points. */
double mf_display_soc(double bms_soc, double rem_ah, double full_ah,
                      double avg_cell_v, double current_a, int voltage_check);

/* Which packs get that voltage check: only JK, and only for now.  Its
 * coulomb counter read 100% at 3.34 V a cell (Sep 8) and should be sound
 * once its cells are top-balanced (planned for October 2026); then this
 * returns 0 for all.  Every other BMS's SOC stands: LFP's curve is so
 * flat that the check misread an XD at 82% as 49% whenever it reported
 * 0 A. */
int mf_soc_voltage_check(const char *driver);

/* System-wide totals over the devices marked active. A device counts only
 * when it is both active and online. Battery power is signed per pack
 * (current_a < 0 = discharging); charge and discharge are summed apart so
 * one pack charging another does not cancel out.
 *
 * Discharge is what leaves the bank: the counted packs' discharge.
 *
 * Load is what the site draws, which the bank, the chargers or the grid
 * may be feeding.  It is the counted inverters' AC output (volts times
 * amps, so grid power passed through counts) when every one of them
 * reports it, phantoms included.  Otherwise it is what must be leaving the
 * DC side: charger input plus the packs' discharge less their charge. */
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
    double discharge_w;              /* set by mf_system_finish() */
    double discharge_max_w;
    double battery_discharge_w;      /* the counted packs' discharge */
    double inverter_output_w;        /* DC the counted inverters draw, inverting */
    int    inverter_output_n;
    double inverter_ac_w;            /* the counted inverters' AC output */
    int    inverter_ac_n;
    double load_w;                   /* set by mf_system_finish() */
    double load_net_w;               /* input + discharge - charge, >= 0 */
    int    load_from_inverters;      /* load_w is inverter_ac_w */
    /* What the counted packs can deliver (their maximum discharge current
     * times voltage) and what the counted inverters are rated for, summed:
     * known only when every one of them reports it (see *_known()). */
    double battery_limit_w;
    int    battery_limit_n;
    int    inverters_total;
    int    inverters_counted;
    double inverter_rated_w;
    int    inverter_rated_n;

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
                           double rem_ah, double full_ah, int cell_count,
                           int voltage_check);
/* A counted pack's maximum discharge in watts, when it reports one; call
 * after mf_system_add_battery() with the same active/online. */
void mf_system_add_battery_limit(mf_system_totals_t *t, bool active,
                                 bool online, double max_discharge_w);
/* rated_w <= 0 means unknown.  output_w is the power it draws from the
 * bank while inverting (0 when it isn't); ac_out_w is its AC output,
 * inverting or passing the grid through.  Either < 0 means unknown. */
void mf_system_add_inverter(mf_system_totals_t *t, bool active, bool online,
                            double rated_w, double output_w, double ac_out_w);
int  mf_system_battery_limit_known(const mf_system_totals_t *t);
int  mf_system_inverter_rated_known(const mf_system_totals_t *t);
int  mf_system_inverter_output_known(const mf_system_totals_t *t);
int  mf_system_inverter_ac_known(const mf_system_totals_t *t);
void mf_system_finish(mf_system_totals_t *t);

#endif
