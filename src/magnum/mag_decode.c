/*
 * Magnum Energy packet decoders.  See mag_decode.h.
 *
 * Derived from pymagnum magnum/inverterdevice.py (InverterDevice.parse and
 * its model/fault/mode/stack tables), remotedevice.py
 * (RemoteDevice.setBaseValues, RemoteDevice.parse), agsdevice.py
 * (AGSDevice.parse, status table), bmkdevice.py (BMKDevice.parse),
 * rtrdevice.py (RTRDevice.parse), pt100device.py (PT100Device.parse and its
 * tables), with the unpack formats of magnum/magnum.py (Magnum.unpackFormats).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 */

#include "mag_decode.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    int         code;
    const char *text;
} code_text_t;

#define LOOKUP(table, code, dflt) lookup(table, sizeof(table) / sizeof(table[0]), \
                                         code, dflt)

static const char *lookup(const code_text_t *t, size_t n, int code,
                          const char *dflt)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (t[i].code == code)
            return t[i].text;
    return dflt;
}

/* InverterDevice.inverter_models */
static const code_text_t k_models[] = {
    { 0x06, "MM612" },     { 0x07, "MM612-AE" },   { 0x08, "MM1212" },
    { 0x09, "MMS1012" },   { 0x0A, "MM1012E" },    { 0x0B, "MM1512" },
    { 0x0C, "MMS912E" },   { 0x0F, "ME1512" },     { 0x14, "ME2012" },
    { 0x15, "RD2212" },    { 0x19, "ME2512" },     { 0x1E, "ME3112" },
    { 0x23, "MS2012" },    { 0x24, "MS1512E" },    { 0x28, "MS2012E" },
    { 0x2C, "MSH3012M" },  { 0x2D, "MS2812" },     { 0x2F, "MS2712E" },
    { 0x35, "MM1324E" },   { 0x36, "MM1524" },     { 0x37, "RD1824" },
    { 0x3B, "RD2624E" },   { 0x3F, "RD2824" },     { 0x45, "RD4024E" },
    { 0x4A, "RD3924" },    { 0x5A, "MS4124E" },    { 0x5B, "MS2024" },
    { 0x67, "MSH4024M" },  { 0x68, "MSH4024RE" },  { 0x69, "MS4024" },
    { 0x6A, "MS4024AE" },  { 0x6B, "MS4024PAE" },  { 0x6F, "MS4448AE" },
    { 0x70, "MS3748AEJ" }, { 0x72, "MS4048" },     { 0x73, "MS4448PAE" },
    { 0x74, "MS3748PAEJ" },{ 0x75, "MS4348PE" },
};

/* InverterDevice.faults */
static const code_text_t k_faults[] = {
    { 0x00, "None" },               { 0x01, "STUCK RELAY" },
    { 0x02, "DC OVERLOAD" },        { 0x03, "AC OVERLOAD" },
    { 0x04, "DEAD BAT" },           { 0x05, "BACKFEED" },
    { 0x08, "LOW BAT" },            { 0x09, "HIGH BAT" },
    { 0x0A, "HIGH AC VOLTS" },      { 0x10, "BAD_BRIDGE" },
    { 0x12, "NTC_FAULT" },          { 0x13, "FET_OVERLOAD" },
    { 0x14, "INTERNAL_FAULT4" },    { 0x16, "STACKER MODE FAULT" },
    { 0x18, "STACKER CLK PH FAULT" }, { 0x17, "STACKER NO CLK FAULT" },
    { 0x19, "STACKER PH LOSS FAULT" }, { 0x20, "OVER TEMP" },
    { 0x21, "RELAY FAULT" },        { 0x80, "CHARGER_FAULT" },
    { 0x81, "High Battery Temp" },  { 0x90, "OPEN SELCO TCO" },
    { 0x91, "CB3 OPEN FAULT" },
};

/* InverterDevice.modes */
static const code_text_t k_modes[] = {
    { 0x00, "Standby" },  { 0x01, "EQ" },       { 0x02, "FLOAT" },
    { 0x04, "ABSORB" },   { 0x08, "BULK" },     { 0x09, "BATSAVER" },
    { 0x10, "CHARGE" },   { 0x20, "Off" },      { 0x40, "INVERT" },
    { 0x50, "Inverter_Standby" }, { 0x80, "SEARCH" },
};

/* InverterDevice.stack_modes */
static const code_text_t k_stack_modes[] = {
    { 0x00, "Stand Alone" },
    { 0x01, "Parallel stack - master" },
    { 0x02, "Parallel stack - slave" },
    { 0x04, "Series stack - master" },
    { 0x08, "Series stack - slave" },
};

/* AGSDevice.status */
static const code_text_t k_ags_status[] = {
    { 0, "Not Connected" },        { 1, "Off" },
    { 2, "Ready" },                { 3, "Manual Run" },
    { 4, "AC In" },                { 5, "In quiet time" },
    { 6, "Start in test mode" },   { 7, "Start on temperature" },
    { 8, "Start on voltage" },     { 9, "Fault start on test" },
    { 10, "Fault start on temp" }, { 11, "Fault start on voltage" },
    { 12, "Start TOD" },           { 13, "Start SOC" },
    { 14, "Start Exercise" },      { 15, "Fault start TOD" },
    { 16, "Fault start SOC" },     { 17, "Fault start Exercise" },
    { 18, "Start on Amp" },        { 19, "Start on Topoff" },
    { 20, "Not used" },            { 21, "Fault start on Amp" },
    { 22, "Fault on Topoff" },     { 23, "Not used" },
    { 24, "Fault max run" },       { 25, "Gen Run Fault" },
    { 26, "Gen in Warm up" },      { 27, "Gen in Cool down" },
};

/* BMKDevice.parse */
static const code_text_t k_bmk_faults[] = {
    { 0, "Reserved" }, { 1, "Normal" }, { 2, "Fault Start" },
};

/* PT100Device.parse */
static const code_text_t k_pt_modes[] = {
    { 1, "Float" }, { 2, "Bulk" }, { 3, "Absorb" }, { 4, "EQ" },
};
static const code_text_t k_pt_regulation[] = {
    { 0, "Off" }, { 1, "Voltage" }, { 2, "Current" }, { 3, "Temperature" },
    { 4, "Hardware" }, { 15, "MPPT" },
};
static const code_text_t k_pt_faults[] = {
    { 0, "No Fault" },             { 1, "Input breaker Fault" },
    { 2, "OPS Fault" },            { 3, "PV High Fault" },
    { 4, "Battery High Fault" },   { 5, "BTS Shorted Fault" },
    { 6, "FET Overtemp Fault" },   { 7, "Battery Overtemp Fault" },
    { 8, "Over Current Fault" },   { 9, "Internal Phase Fault" },
    { 10, "Open BTS Fault" },      { 11, "Internal Fault 1" },
    { 12, "GFP Fault" },           { 13, "ARC Fault" },
    { 14, "NTC Fault" },           { 15, "HW overtemp Fault" },
    { 16, "Overyemp Fault" },      { 17, "USB Fault" },
    { 20, "Stack Fault" },         { 21, "Stack warning" },
    { 22, "Stack DIP Fault" },     { 23, "Stack DT Fault" },
};

int mag_model_known(int model)
{
    return LOOKUP(k_models, model, NULL) != NULL;
}

int mag_multiplier(int model)
{
    if (model <= 50)
        return 1;
    if (model <= 107)
        return 2;
    return 4;                           /* pymagnum: <= 150; above, unchanged */
}

const char *mag_mode_text(int mode)        { return LOOKUP(k_modes, mode, "??"); }
const char *mag_fault_text(int fault)      { return LOOKUP(k_faults, fault, "Unknown"); }
const char *mag_model_text(int model)      { return LOOKUP(k_models, model, "Unknown"); }
const char *mag_stackmode_text(int sm)     { return LOOKUP(k_stack_modes, sm, "Unknown"); }
const char *mag_ags_status_text(int st)    { return LOOKUP(k_ags_status, st, "Unknown"); }
const char *mag_bmk_fault_text(int fault)  { return LOOKUP(k_bmk_faults, fault, "Unknown"); }
const char *mag_pt_mode_text(int mode)     { return LOOKUP(k_pt_modes, mode, "Unknown"); }
const char *mag_pt_regulation_text(int r)  { return LOOKUP(k_pt_regulation, r, "Unknown"); }
const char *mag_pt_fault_text(int fault)   { return LOOKUP(k_pt_faults, fault, "unknown"); }

static int u16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static int s16(const uint8_t *p)
{
    return (int16_t)(uint16_t)((p[0] << 8) | p[1]);
}

static int s8(uint8_t b)
{
    return (int8_t)b;
}

/* pymagnum rounds some values; keep numbers tidy the same way. */
static double r(double v, int dec)
{
    double k = dec == 1 ? 10.0 : dec == 2 ? 100.0 : 1.0;

    return round(v * k) / k;
}

/* A delay byte over 127 is minutes in its low nibble (RemoteDevice.parse). */
static int delay_s(int v)
{
    return v > 127 ? (v & 0x0F) * 60 : v;
}

/* ---- inverter (InverterDevice.parse; format 'BBhhBBBBBBBBBBBBhb') ---- */

static int decode_inverter(mag_state_t *s, const uint8_t *p, double now)
{
    mag_inverter_t *v = &s->inv;

    /* One tap, one inverter: follow the first stack role seen (issue #5's
     * byte 15) and count packets from any other. */
    if (!s->stack_locked || (v->seen && now - v->t > MAG_STACK_RELOCK_S))
    {
        s->stack_locked = 1;
        s->stack_lock = p[15];
    }
    if (p[15] != s->stack_lock)
    {
        s->other_inverter_packets++;
        return 0;
    }
    v->seen = 1;
    v->t = now;
    v->mode = p[0];
    v->fault = p[1];
    v->dc_voltage_v = r(s16(p + 2) / 10.0, 1);
    v->dc_current_a = s16(p + 4);
    v->ac_out_v = p[6];
    v->ac_in_v = p[7];
    v->invled = p[8] != 0;
    v->chgled = p[9] != 0;
    v->revision = r(p[10] / 10.0, 1);
    v->bat_temp_c = p[11];
    v->tfmr_temp_c = p[12];
    v->fet_temp_c = p[13];
    v->model = p[14];
    v->stackmode = p[15];
    v->ac_in_a = p[16];
    v->ac_out_a = p[17];
    v->freq_hz = r(s16(p + 18) / 10.0, 1);
    if (v->model <= 150)
        s->mult = mag_multiplier(v->model);
    return 1;
}

/* ---- remote (RemoteDevice; base format 'BBBBBbBBBBBBBB' + 7 bytes) ---- */

static void remote_base(mag_state_t *s, const uint8_t *p)
{
    mag_remote_t *v = &s->rem;
    int b3 = p[3];

    v->search_w = p[1];
    if (b3 > 100)
    {
        v->absorb_v = r(b3 * s->mult / 10.0, 1);
        v->battery_type = 0;
    }
    else
    {
        v->absorb_v = 0;
        v->battery_type = b3;
    }
    v->charge_rate = p[4];
    v->ac_input_a = s8(p[5]);
    v->revision = r(p[6] / 10.0, 1);
    v->parallel = (p[7] & 0x0F) * 10;
    v->lbco_v = r(p[9] / 10.0, 1);
    v->ac_cutout_v = p[10];
    v->float_v = r(p[11] * s->mult / 10.0, 1);
    v->eq_v = r(v->absorb_v + p[12] / 10.0, 1);
    v->absorb_time_h = r(p[13] / 10.0, 1);
}

static void decode_remote(mag_state_t *s, const uint8_t *p)
{
    mag_remote_t *v = &s->rem;

    remote_base(s, p);
    switch (p[20])
    {
    case 0x80:                          /* 'bbbb3B': BMK settings */
        v->have_80 = 1;
        v->battery_size = p[2] * 10;
        v->have_clock = 1;
        v->clock_min = s8(p[14]) * 60 + s8(p[15]);
        v->battery_efficiency_pct = s8(p[16]);
        break;
    case 0xA0:                          /* 'BBBbBBB' */
        v->have_a0 = 1;
        v->have_clock = 1;
        v->clock_min = p[14] * 60 + p[15];
        v->gen_run_time_h = r(p[16] / 10.0, 1);
        v->gen_start_temp_c = r((s8(p[17]) - 32) * 5.0 / 9.0, 1);
        v->gen_start_v = r(p[18] * s->mult / 10.0, 1);
        v->quiet_time = p[19];
        break;
    case 0xA1:
        v->have_a1 = 1;
        v->gen_start_time_min = p[14] * 15;
        v->gen_stop_time_min = p[15] * 15;
        v->gen_stop_v = r(p[16] * s->mult / 10.0, 1);
        v->gen_volt_start_delay_s = delay_s(p[17]);
        v->gen_volt_stop_delay_s = delay_s(p[18]);
        v->gen_max_run_h = r(p[19] / 10.0, 1);
        break;
    case 0xA2:                          /* 'bb5B' */
        v->have_a2 = 1;
        v->gen_soc_start_pct = s8(p[14]);
        v->gen_soc_stop_pct = s8(p[15]);
        v->gen_amp_start_a = p[16];
        v->gen_amp_start_delay_s = delay_s(p[17]);
        v->gen_amp_stop_a = p[18];
        v->gen_amp_stop_delay_s = delay_s(p[19]);
        break;
    case 0xA3:
        v->have_a3 = 1;
        v->quiet_begin_min = p[14] * 15;
        v->quiet_end_min = p[15] * 15;
        v->exercise_start_min = p[16] * 15;
        v->exercise_run_time_h = r(p[17] / 10.0, 1);
        v->gen_topoff = p[18];
        break;
    case 0xA4:
        v->have_a4 = 1;
        v->gen_warmup_s = delay_s(p[14]);
        v->gen_cooldown_s = delay_s(p[15]);
        break;
    default:                            /* 00, 11, C0-C3, D0: base only */
        break;
    }
}

/* ---- AGS (AGSDevice.parse; A1 'BbBbBB', A2 'BBBHB') ---- */

static void decode_ags_a1(mag_state_t *s, const uint8_t *p)
{
    mag_ags_t *v = &s->ags;
    int st = s8(p[1]), temp = s8(p[3]);

    v->have_a1 = 1;
    v->status = st;
    v->running = st == 3 || st == 6 || st == 7 || st == 8 || st == 12 ||
                 st == 13 || st == 14 || st == 18 || st == 19 || st == 26 ||
                 st == 27;
    v->revision = r(p[2] / 10.0, 1);
    v->temp_valid = temp < 105;
    v->temp_c = v->temp_valid ? r((temp - 32) * 5.0 / 9.0, 1) : 0;
    v->run_time_h = r(p[4] / 10.0, 1);
    v->battery_voltage_v = r(p[5] / 10.0 * s->mult, 1);
}

static void decode_ags_a2(mag_state_t *s, const uint8_t *p)
{
    mag_ags_t *v = &s->ags;

    v->have_a2 = 1;
    v->last_run = p[1];
    v->last_full_soc = p[2];
    v->total_run = u16(p + 3);
}

/* ---- BMK (BMKDevice.parse; 'BbHhHHhHHBB') ---- */

static void decode_bmk(mag_state_t *s, const uint8_t *p)
{
    mag_bmk_t *v = &s->bmk;

    v->soc_pct = s8(p[1]);
    v->battery_voltage_v = r(u16(p + 2) / 100.0, 2);
    v->battery_current_a = r(s16(p + 4) / 10.0, 1);
    v->battery_voltage_min_v = r(u16(p + 6) / 100.0, 2);
    v->battery_voltage_max_v = r(u16(p + 8) / 100.0, 2);
    v->net_ah = s16(p + 10);
    v->trip_ah = r(u16(p + 12) / 10.0, 1);
    v->lifetime_ah = u16(p + 14) * 100;
    v->revision = r(p[16] / 10.0, 1);
    v->fault = p[17];
}

/* ---- PT-100 (PT100Device.parse; C1 'BBBBHhHBBBBBB', C2 'BBHHBBBBBBB') --- */

static void decode_pt_c1(mag_state_t *s, const uint8_t *p)
{
    mag_pt100_t *v = &s->pt;

    v->have_c1 = 1;
    v->address = p[1] & 0x07;
    v->mode = p[2] & 0x0F;
    v->regulation = (p[2] >> 4) & 0x0F;
    v->fault = p[3] >> 3;
    v->battery_voltage_v = r(u16(p + 4) / 10.0, 1);
    v->battery_current_a = r(s16(p + 6) / 10.0, 1);
    v->pv_voltage_v = r(u16(p + 8) / 10.0, 1);
    v->charge_time = r(p[10] / 10.0, 1);
    v->target_voltage_v = r(p[11] / 10.0 * s->mult, 1);
    v->relay_on = p[12] & 0x01;
    v->alarm_on = (p[12] >> 1) & 0x01;
    v->fan_on = (p[12] >> 3) & 0x01;
    v->is_day = (p[12] >> 4) & 0x01;
    v->bat_temp_valid = !(p[13] == 0x97 || p[13] == 0x98);  /* short/open */
    v->bat_temp_c = p[13];
    v->inductor_temp_c = p[14];
    v->fet_temp_c = p[15];
}

static void decode_pt_c2(mag_state_t *s, const uint8_t *p)
{
    mag_pt100_t *v = &s->pt;
    int nb = p[7] & 0x03;

    v->have_c2 = 1;
    v->lifetime_kwh = u16(p + 2) * 10;
    v->resettable_kwh = r(u16(p + 4) / 10.0, 1);
    v->ground_fault_current = p[6];
    v->stacker_info = p[7] >> 2;
    v->nominal_voltage_v = nb == 0 ? 12.0 : nb * 24.0;
    v->dip_switches = p[8];
    v->revision = r(p[9] / 10.0, 1);
    v->output_current_rating_a = p[10];
    v->input_voltage_rating_v = p[11] * 10;
}

void mag_state_init(mag_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mult = 1;                        /* pymagnum's default */
}

int mag_decode(mag_state_t *s, mag_pkt_t type, const uint8_t *p, size_t len,
               double now)
{
    switch (type)
    {
    case MAG_PKT_INVERTER:
        return len >= MAG_INV_LEN ? decode_inverter(s, p, now) : 0;
    case MAG_PKT_REMOTE:
        if (len < MAG_REMOTE_LEN)
            return 0;
        decode_remote(s, p);
        s->rem.seen = 1;
        s->rem.t = now;
        break;
    case MAG_PKT_AGS_A1:
    case MAG_PKT_AGS_A2:
        if (len < 6)
            return 0;
        if (type == MAG_PKT_AGS_A1)
            decode_ags_a1(s, p);
        else
            decode_ags_a2(s, p);
        s->ags.seen = 1;
        s->ags.t = now;
        break;
    case MAG_PKT_BMK_81:
        if (len < 18)
            return 0;
        decode_bmk(s, p);
        s->bmk.seen = 1;
        s->bmk.t = now;
        break;
    case MAG_PKT_RTR_91:
        if (len < 2)
            return 0;
        s->rtr.revision = r(p[1] / 10.0, 1);
        s->rtr.seen = 1;
        s->rtr.t = now;
        break;
    case MAG_PKT_PT_C1:
    case MAG_PKT_PT_C2:
    case MAG_PKT_PT_C3:
        /* pymagnum decodes address 0 only, and never C3. */
        if (type == MAG_PKT_PT_C1 && len >= 16 && (p[1] & 0x07) == 0)
            decode_pt_c1(s, p);
        else if (type == MAG_PKT_PT_C2 && len >= 13 && (p[1] & 0x07) == 0)
            decode_pt_c2(s, p);
        s->pt.seen = 1;
        s->pt.t = now;
        break;
    case MAG_PKT_ACLD_D1:
        s->acld.seen = 1;               /* pymagnum does not decode it */
        s->acld.t = now;
        break;
    default:
        break;
    }
    return 0;
}
