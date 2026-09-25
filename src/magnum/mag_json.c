/*
 * A Magnum tap's reading as JSON.  See mag_json.h.
 *
 * Built from pymagnum's per-device data (magnum/inverterdevice.py,
 * remotedevice.py incl. RemoteDevice.cleanup, agsdevice.py, bmkdevice.py,
 * rtrdevice.py, pt100device.py), with moon-flare field names.
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 */

#include "mag_json.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void num(cJSON *o, const char *k, double v)
{
    cJSON_AddNumberToObject(o, k, v);
}

static void age(cJSON *o, double t, double now)
{
    num(o, "last_seen_s", round((now - t) * 10.0) / 10.0);
}

/* A clock time, pymagnum's hhmm integer as "HH:MM". */
static void clock_text(cJSON *o, const char *k, int minutes)
{
    char buf[8];

    snprintf(buf, sizeof(buf), "%02d:%02d", (minutes / 60) % 100, minutes % 60);
    cJSON_AddStringToObject(o, k, buf);
}

static void add_inverter(cJSON *o, const mag_inverter_t *v)
{
    num(o, "mode", v->mode);
    cJSON_AddStringToObject(o, "mode_text", mag_mode_text(v->mode));
    num(o, "fault", v->fault);
    cJSON_AddStringToObject(o, "fault_text", mag_fault_text(v->fault));
    num(o, "dc_voltage_v", v->dc_voltage_v);
    num(o, "dc_current_a", v->dc_current_a);
    /* Derived: DC volts x amps (the inverter's DC volts are coarse). */
    num(o, "dc_power_w", round(v->dc_voltage_v * v->dc_current_a * 10.0) / 10.0);
    num(o, "ac_out_v", v->ac_out_v);
    num(o, "ac_out_a", v->ac_out_a);
    num(o, "ac_in_v", v->ac_in_v);
    num(o, "ac_in_a", v->ac_in_a);
    num(o, "freq_hz", v->freq_hz);
    cJSON_AddBoolToObject(o, "invled", v->invled);
    cJSON_AddBoolToObject(o, "chgled", v->chgled);
    num(o, "bat_temp_c", v->bat_temp_c);
    num(o, "tfmr_temp_c", v->tfmr_temp_c);
    num(o, "fet_temp_c", v->fet_temp_c);
    num(o, "model", v->model);
    cJSON_AddStringToObject(o, "model_text", mag_model_text(v->model));
    num(o, "stackmode", v->stackmode);
    cJSON_AddStringToObject(o, "stackmode_text", mag_stackmode_text(v->stackmode));
    num(o, "revision", v->revision);
}

/* AGS settings appear only when an AGS has answered, the BMK's only when a
 * BMK has (RemoteDevice.cleanup). */
static cJSON *remote_obj(const mag_state_t *s, double now)
{
    const mag_remote_t *v = &s->rem;
    cJSON *o = cJSON_CreateObject();

    age(o, v->t, now);
    num(o, "revision", v->revision);
    num(o, "search_w", v->search_w);
    num(o, "battery_type", v->battery_type);
    num(o, "absorb_v", v->absorb_v);
    num(o, "float_v", v->float_v);
    num(o, "eq_v", v->eq_v);
    num(o, "absorb_time_h", v->absorb_time_h);
    num(o, "charge_rate", v->charge_rate);
    num(o, "ac_input_a", v->ac_input_a);
    num(o, "ac_cutout_v", v->ac_cutout_v);
    num(o, "lbco_v", v->lbco_v);
    num(o, "parallel", v->parallel);
    if (v->have_clock)
        clock_text(o, "clock_time", v->clock_min);
    else
        cJSON_AddNullToObject(o, "clock_time");
    if (v->have_80)
        num(o, "battery_size", v->battery_size);
    else
        cJSON_AddNullToObject(o, "battery_size");
    if (s->bmk.seen && v->have_80)
        num(o, "battery_efficiency_pct", v->battery_efficiency_pct);
    if (!s->ags.seen)
        return o;
    if (v->have_a0)
    {
        num(o, "gen_run_time_h", v->gen_run_time_h);
        num(o, "gen_start_temp_c", v->gen_start_temp_c);
        num(o, "gen_start_v", v->gen_start_v);
        num(o, "quiet_time", v->quiet_time);
    }
    if (v->have_a1)
    {
        clock_text(o, "gen_start_time", v->gen_start_time_min);
        clock_text(o, "gen_stop_time", v->gen_stop_time_min);
        num(o, "gen_stop_v", v->gen_stop_v);
        num(o, "gen_volt_start_delay_s", v->gen_volt_start_delay_s);
        num(o, "gen_volt_stop_delay_s", v->gen_volt_stop_delay_s);
        num(o, "gen_max_run_h", v->gen_max_run_h);
    }
    if (v->have_a2)
    {
        num(o, "gen_soc_start_pct", v->gen_soc_start_pct);
        num(o, "gen_soc_stop_pct", v->gen_soc_stop_pct);
        num(o, "gen_amp_start_a", v->gen_amp_start_a);
        num(o, "gen_amp_start_delay_s", v->gen_amp_start_delay_s);
        num(o, "gen_amp_stop_a", v->gen_amp_stop_a);
        num(o, "gen_amp_stop_delay_s", v->gen_amp_stop_delay_s);
    }
    if (v->have_a3)
    {
        clock_text(o, "quiet_begin_time", v->quiet_begin_min);
        clock_text(o, "quiet_end_time", v->quiet_end_min);
        clock_text(o, "exercise_start_time", v->exercise_start_min);
        num(o, "exercise_run_time_h", v->exercise_run_time_h);
        num(o, "gen_topoff", v->gen_topoff);
    }
    if (v->have_a4)
    {
        num(o, "gen_warmup_s", v->gen_warmup_s);
        num(o, "gen_cooldown_s", v->gen_cooldown_s);
    }
    return o;
}

static cJSON *ags_obj(const mag_ags_t *v, double now)
{
    cJSON *o = cJSON_CreateObject();

    age(o, v->t, now);
    if (v->have_a1)
    {
        num(o, "revision", v->revision);
        num(o, "status", v->status);
        cJSON_AddStringToObject(o, "status_text", mag_ags_status_text(v->status));
        cJSON_AddBoolToObject(o, "running", v->running);
        if (v->temp_valid)
            num(o, "temp_c", v->temp_c);
        else
            cJSON_AddNullToObject(o, "temp_c");
        num(o, "run_time_h", v->run_time_h);
        num(o, "battery_voltage_v", v->battery_voltage_v);
    }
    if (v->have_a2)
    {
        num(o, "last_run", v->last_run);
        num(o, "last_full_soc", v->last_full_soc);
        num(o, "total_run", v->total_run);
    }
    return o;
}

static cJSON *bmk_obj(const mag_bmk_t *v, double now)
{
    cJSON *o = cJSON_CreateObject();

    age(o, v->t, now);
    num(o, "revision", v->revision);
    num(o, "soc_pct", v->soc_pct);
    num(o, "battery_voltage_v", v->battery_voltage_v);
    num(o, "battery_current_a", v->battery_current_a);
    num(o, "battery_voltage_min_v", v->battery_voltage_min_v);
    num(o, "battery_voltage_max_v", v->battery_voltage_max_v);
    num(o, "net_ah", v->net_ah);
    num(o, "trip_ah", v->trip_ah);
    num(o, "lifetime_ah", v->lifetime_ah);
    num(o, "fault", v->fault);
    cJSON_AddStringToObject(o, "fault_text", mag_bmk_fault_text(v->fault));
    return o;
}

static cJSON *pt100_obj(const mag_pt100_t *v, double now)
{
    cJSON *o = cJSON_CreateObject();

    age(o, v->t, now);
    if (v->have_c1)
    {
        char bits[9];
        int i;

        num(o, "address", v->address);
        num(o, "mode", v->mode);
        cJSON_AddStringToObject(o, "mode_text", mag_pt_mode_text(v->mode));
        num(o, "regulation", v->regulation);
        cJSON_AddStringToObject(o, "regulation_text",
                                mag_pt_regulation_text(v->regulation));
        num(o, "fault", v->fault);
        cJSON_AddStringToObject(o, "fault_text", mag_pt_fault_text(v->fault));
        num(o, "battery_voltage_v", v->battery_voltage_v);
        num(o, "battery_current_a", v->battery_current_a);
        num(o, "pv_voltage_v", v->pv_voltage_v);
        num(o, "charge_time", v->charge_time);
        num(o, "target_voltage_v", v->target_voltage_v);
        cJSON_AddBoolToObject(o, "relay_on", v->relay_on);
        cJSON_AddBoolToObject(o, "alarm_on", v->alarm_on);
        cJSON_AddBoolToObject(o, "fan_on", v->fan_on);
        cJSON_AddBoolToObject(o, "is_day", v->is_day);
        if (v->bat_temp_valid)
            num(o, "bat_temp_c", v->bat_temp_c);
        else
            cJSON_AddNullToObject(o, "bat_temp_c");
        num(o, "inductor_temp_c", v->inductor_temp_c);
        num(o, "fet_temp_c", v->fet_temp_c);
        if (v->have_c2)
        {
            num(o, "revision", v->revision);
            num(o, "lifetime_kwh", v->lifetime_kwh);
            num(o, "resettable_kwh", v->resettable_kwh);
            num(o, "ground_fault_current", v->ground_fault_current);
            num(o, "nominal_voltage_v", v->nominal_voltage_v);
            num(o, "stacker_info", v->stacker_info);
            for (i = 0; i < 8; i++)
                bits[i] = (v->dip_switches >> (7 - i)) & 1 ? '1' : '0';
            bits[8] = '\0';
            cJSON_AddStringToObject(o, "dip_switches", bits);
            num(o, "output_current_rating_a", v->output_current_rating_a);
            num(o, "input_voltage_rating_v", v->input_voltage_rating_v);
        }
    }
    return o;
}

static cJSON *diag_obj(const mag_state_t *s, const mag_frame_stats_t *st,
                       const char *port, double now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *pk = cJSON_CreateObject();

    num(o, "bytes_read", (double)st->bytes_read);
    num(pk, "inverter", (double)st->packets[MAG_PKT_INVERTER]);
    num(pk, "remote", (double)st->packets[MAG_PKT_REMOTE]);
    num(pk, "router", (double)st->packets[MAG_PKT_RTR_91]);
    num(pk, "ags", (double)(st->packets[MAG_PKT_AGS_A1] + st->packets[MAG_PKT_AGS_A2]));
    num(pk, "bmk", (double)st->packets[MAG_PKT_BMK_81]);
    num(pk, "pt100", (double)(st->packets[MAG_PKT_PT_C1] + st->packets[MAG_PKT_PT_C2] +
                              st->packets[MAG_PKT_PT_C3]));
    num(pk, "acld", (double)st->packets[MAG_PKT_ACLD_D1]);
    cJSON_AddItemToObject(o, "packets", pk);
    num(o, "bad_packets", (double)st->bad_packets);
    num(o, "unknown_bytes", (double)st->unknown_bytes);
    num(o, "resyncs", (double)st->resyncs);
    num(o, "ff_bytes", (double)st->ff_bytes);
    num(o, "other_inverter_packets", (double)s->other_inverter_packets);
    if (s->inv.seen)
        num(o, "last_inverter_s", round((now - s->inv.t) * 10.0) / 10.0);
    else
        cJSON_AddNullToObject(o, "last_inverter_s");
    if (port && port[0])
        cJSON_AddStringToObject(o, "port", port);
    else
        cJSON_AddNullToObject(o, "port");
    return o;
}

char *mag_reading_json(const mag_state_t *s, const mag_frame_stats_t *st,
                       const char *tap, const char *port, double now)
{
    static const char *const trim[] = { "pt100", "bmk", "remote" };
    cJSON *o = cJSON_CreateObject();
    char *out;
    size_t i;

    if (!o)
        return NULL;
    if (tap && tap[0])
        cJSON_AddStringToObject(o, "tap", tap);
    else
        cJSON_AddNullToObject(o, "tap");
    add_inverter(o, &s->inv);
    if (s->rem.seen)
        cJSON_AddItemToObject(o, "remote", remote_obj(s, now));
    if (s->rtr.seen)
    {
        cJSON *r = cJSON_CreateObject();

        age(r, s->rtr.t, now);
        num(r, "revision", s->rtr.revision);
        cJSON_AddItemToObject(o, "router", r);
    }
    if (s->ags.seen)
        cJSON_AddItemToObject(o, "ags", ags_obj(&s->ags, now));
    if (s->bmk.seen)
        cJSON_AddItemToObject(o, "bmk", bmk_obj(&s->bmk, now));
    if (s->pt.seen)
        cJSON_AddItemToObject(o, "pt100", pt100_obj(&s->pt, now));
    if (s->acld.seen)
    {
        cJSON *a = cJSON_CreateObject();

        age(a, s->acld.t, now);
        cJSON_AddItemToObject(o, "acld", a);
    }
    cJSON_AddItemToObject(o, "diag", diag_obj(s, st, port, now));
    out = cJSON_PrintUnformatted(o);
    /* History keeps readings under 4 KB: shed the least useful first. */
    for (i = 0; out && strlen(out) > MAG_READING_MAX && i < 3; i++)
    {
        free(out);
        cJSON_DeleteItemFromObjectCaseSensitive(o, trim[i]);
        if (i == 0)
            cJSON_AddBoolToObject(cJSON_GetObjectItemCaseSensitive(o, "diag"),
                                  "trimmed", 1);
        out = cJSON_PrintUnformatted(o);
    }
    cJSON_Delete(o);
    return out;
}
