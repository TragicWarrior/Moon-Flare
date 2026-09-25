#ifndef MF_MAG_DECODE_H
#define MF_MAG_DECODE_H

/*
 * Magnum Energy packet decoders and the state they build.
 *
 * Derived from pymagnum magnum/inverterdevice.py, remotedevice.py,
 * agsdevice.py, bmkdevice.py, rtrdevice.py, pt100device.py (the *.parse()
 * methods and their enum tables).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 *
 * Offsets and scaling follow pymagnum; field names follow moon-flare
 * (units as suffixes; README "Magnum inverters" maps them to pymagnum's).
 * Deviations from pymagnum:
 *   - The voltage multiplier (12/24/48 V) belongs to the tap's state, not a
 *     class-wide global, and is known before other packets are decoded.
 *   - REMOTE_A3 byte 17 is exercise_run_time_h; pymagnum stores it over
 *     A0's "runtime".
 *   - Router revision keeps its decimal (3.2); pymagnum rounds to 3.
 *   - An AGS temperature of 105 or more (not converted by pymagnum) is null.
 *   - Unknown fault codes read "Unknown" instead of keeping stale text.
 */

#include "mag_frame.h"

#include <stdint.h>

/* The tap follows the stack role of the first inverter packet it sees;
 * after this long without one, it follows the next role it sees. */
#define MAG_STACK_RELOCK_S 5.0

typedef struct {
    int    seen;
    double t;
    int    mode;
    int    fault;
    double dc_voltage_v;
    double dc_current_a;
    int    ac_out_v;
    int    ac_in_v;             /* peak-to-peak, per pymagnum */
    int    ac_in_a;
    int    ac_out_a;
    int    invled;
    int    chgled;
    double revision;
    int    bat_temp_c;
    int    tfmr_temp_c;
    int    fet_temp_c;
    int    model;
    int    stackmode;
    double freq_hz;
} mag_inverter_t;

typedef struct {
    int    seen;
    double t;
    /* every remote packet */
    double revision;
    int    search_w;
    int    battery_type;
    double absorb_v;
    int    charge_rate;
    int    ac_input_a;
    int    parallel;
    double lbco_v;
    int    ac_cutout_v;
    double float_v;
    double eq_v;
    double absorb_time_h;
    /* REMOTE_80 */
    int    have_80;
    int    battery_size;
    int    battery_efficiency_pct;
    int    have_clock;          /* REMOTE_80 / REMOTE_A0 */
    int    clock_min;
    /* REMOTE_A0..A4: AGS settings */
    int    have_a0;
    double gen_run_time_h;
    double gen_start_temp_c;
    double gen_start_v;
    int    quiet_time;
    int    have_a1;
    int    gen_start_time_min;
    int    gen_stop_time_min;
    double gen_stop_v;
    int    gen_volt_start_delay_s;
    int    gen_volt_stop_delay_s;
    double gen_max_run_h;
    int    have_a2;
    int    gen_soc_start_pct;
    int    gen_soc_stop_pct;
    int    gen_amp_start_a;
    int    gen_amp_start_delay_s;
    int    gen_amp_stop_a;
    int    gen_amp_stop_delay_s;
    int    have_a3;
    int    quiet_begin_min;
    int    quiet_end_min;
    int    exercise_start_min;
    double exercise_run_time_h;
    int    gen_topoff;
    int    have_a4;
    int    gen_warmup_s;
    int    gen_cooldown_s;
} mag_remote_t;

typedef struct {
    int    seen;
    double t;
    int    have_a1;
    int    have_a2;
    double revision;
    int    status;
    int    running;
    int    temp_valid;
    double temp_c;
    double run_time_h;
    double battery_voltage_v;
    int    last_run;
    int    last_full_soc;
    int    total_run;
} mag_ags_t;

typedef struct {
    int    seen;
    double t;
    double revision;
    int    soc_pct;
    double battery_voltage_v;
    double battery_current_a;
    double battery_voltage_min_v;
    double battery_voltage_max_v;
    int    net_ah;
    double trip_ah;
    int    lifetime_ah;
    int    fault;
} mag_bmk_t;

typedef struct {
    int    seen;
    double t;
    double revision;
} mag_rtr_t;

typedef struct {
    int    seen;
    double t;
    int    have_c1;
    int    have_c2;
    int    address;
    int    mode;
    int    regulation;
    int    fault;
    double battery_voltage_v;
    double battery_current_a;
    double pv_voltage_v;
    double charge_time;
    double target_voltage_v;
    int    relay_on;
    int    alarm_on;
    int    fan_on;
    int    is_day;
    int    bat_temp_valid;
    int    bat_temp_c;
    int    inductor_temp_c;
    int    fet_temp_c;
    int    lifetime_kwh;
    double resettable_kwh;
    int    ground_fault_current;
    int    stacker_info;
    double nominal_voltage_v;
    int    dip_switches;
    double revision;
    int    output_current_rating_a;
    int    input_voltage_rating_v;
} mag_pt100_t;

typedef struct {
    int    seen;
    double t;
} mag_acld_t;

typedef struct {
    mag_inverter_t inv;
    mag_remote_t   rem;
    mag_ags_t      ags;
    mag_bmk_t      bmk;
    mag_rtr_t      rtr;
    mag_pt100_t    pt;
    mag_acld_t     acld;
    int      mult;                  /* 1, 2 or 4: 12/24/48 V system */
    int      stack_locked;
    int      stack_lock;
    uint64_t other_inverter_packets;
} mag_state_t;

void mag_state_init(mag_state_t *s);
/* Decode one framed packet.  Returns 1 when it updated the inverter. */
int  mag_decode(mag_state_t *s, mag_pkt_t type, const uint8_t *p, size_t len,
                double now);

int  mag_model_known(int model);
int  mag_multiplier(int model);         /* pymagnum: <=50 x1, <=107 x2, <=150 x4 */

const char *mag_mode_text(int mode);
const char *mag_fault_text(int fault);
const char *mag_model_text(int model);
const char *mag_stackmode_text(int stackmode);
const char *mag_ags_status_text(int status);
const char *mag_bmk_fault_text(int fault);
const char *mag_pt_mode_text(int mode);
const char *mag_pt_regulation_text(int regulation);
const char *mag_pt_fault_text(int fault);

#endif
