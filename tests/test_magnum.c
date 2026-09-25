/*
 * Magnum framing, decoding and reading.  Expected values are pymagnum's own
 * decode of its testdata/allpackets.txt (plus an MS4448PAE inverter packet,
 * so the 48 V multiplier applies), run through pymagnum's device classes in
 * bus order; deviations from pymagnum are marked.
 */

#define _GNU_SOURCE

#include "mag_decode.h"
#include "mag_frame.h"
#include "mag_json.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-6)

/* ---- packets ------------------------------------------------------- */

static const char *INV48 = "4000020E0016780001003D11332473010005025800";  /* master */
static const char *INV48_SLAVE = "4000020E0016780001003D11332473020005025800";
static const char *INV48_STANDBY = "0000020E0000780001003D11332473010000025800";
static const char *REM_A0 = "00002808640A280000C89B840C14122014007300A0";
static const char *REM_A1 = "00002808640A280000C89B840C14600090787878A1";
static const char *REM_A2 = "00003C04500F170601C8A5860100465500781478A2";
static const char *REM_A3 = "00003C04500F170601C8A58601005E5F00240200A3";
static const char *REM_A4 = "00003C04500F170601C8A58601001E1E00000000A4";
static const char *REM_80 = "00002808640A280000C89B840C1412200000280080";
static const char *REM_00 = "00002808640A280000C89B840C1412200000000000";
static const char *REM_C1 = "00002808640A280000C89B840C14000000000000C1";
static const char *REM_C2 = "00002808640A280000C89B840C14000000000000C2";
static const char *AGS_A1 = "A102343A007F";
static const char *AGS_A2 = "A20000000000";
static const char *BMK_81 = "814C09F1007407E00C08FF984FF000140A01";
static const char *RTR_91 = "9120";
static const char *PT_C1 = "C1001300024D008505BC1194101B313E";
static const char *PT_C2 = "C200000200AC0002050B641425";

typedef struct {
    uint8_t b[16384];
    size_t  n;
} stream_t;

static void put(stream_t *s, const char *hex)
{
    while (hex[0] && hex[1] && s->n < sizeof(s->b))
    {
        unsigned v;

        sscanf(hex, "%2x", &v);
        s->b[s->n++] = (uint8_t)v;
        hex += 2;
    }
}

static void put_bytes(stream_t *s, const uint8_t *p, size_t n)
{
    memcpy(s->b + s->n, p, n);
    s->n += n;
}

/* The pymagnum sample network: one cycle per peripheral packet. */
static void sample_network(stream_t *s)
{
    put(s, INV48); put(s, REM_A0); put(s, AGS_A1);
    put(s, INV48); put(s, REM_A1); put(s, AGS_A2);
    put(s, INV48); put(s, REM_A2);
    put(s, INV48); put(s, REM_A3);
    put(s, INV48); put(s, REM_A4);
    put(s, INV48); put(s, REM_80); put(s, BMK_81);
    put(s, INV48); put(s, REM_C1); put(s, PT_C1);
    put(s, INV48); put(s, REM_C2); put(s, PT_C2);
    put(s, INV48); put(s, REM_00); put(s, RTR_91);
}

typedef struct {
    mag_frame_t f;
    mag_state_t s;
    int         types[64];
    int         ntypes;
} rig_t;

static void on_packet(void *arg, mag_pkt_t t, const uint8_t *p, size_t len,
                      double now)
{
    rig_t *r = arg;

    if (t != MAG_PKT_UNKNOWN && r->ntypes < 64)
        r->types[r->ntypes++] = (int)t;
    (void)mag_decode(&r->s, t, p, len, now);
}

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    mag_frame_init(&r->f, on_packet, r);
    mag_state_init(&r->s);
}

/* Feed in chunks of `chunk` bytes (0: all at once), 10 ms apart. */
static void feed(rig_t *r, const stream_t *s, size_t chunk)
{
    size_t off = 0;
    double t = 1.0;

    if (!chunk)
        chunk = s->n;
    while (off < s->n)
    {
        size_t n = s->n - off < chunk ? s->n - off : chunk;

        mag_frame_feed(&r->f, s->b + off, n, t);
        off += n;
        t += 0.010;
    }
    mag_frame_idle(&r->f, t + 1.0);
}

/* ---- decoding: pymagnum's values ---------------------------------- */

static void check_sample_values(const rig_t *r)
{
    const mag_state_t *s = &r->s;

    /* INVERTER */
    CHECK(s->inv.seen, "inverter seen");
    CHECK(s->inv.mode == 64 && strcmp(mag_mode_text(s->inv.mode), "INVERT") == 0, "mode");
    CHECK(s->inv.fault == 0 && strcmp(mag_fault_text(0), "None") == 0, "fault");
    CHECK(NEAR(s->inv.dc_voltage_v, 52.6), "vdc");
    CHECK(NEAR(s->inv.dc_current_a, 22.0), "adc");
    CHECK(s->inv.ac_out_v == 120 && s->inv.ac_in_v == 0, "VACout/VACin");
    CHECK(s->inv.invled == 1 && s->inv.chgled == 0, "leds");
    CHECK(NEAR(s->inv.revision, 6.1), "revision");
    CHECK(s->inv.bat_temp_c == 17 && s->inv.tfmr_temp_c == 51 && s->inv.fet_temp_c == 36, "temps");
    CHECK(s->inv.model == 115 && strcmp(mag_model_text(115), "MS4448PAE") == 0, "model");
    CHECK(s->inv.stackmode == 1 &&
          strcmp(mag_stackmode_text(1), "Parallel stack - master") == 0, "stackmode");
    CHECK(s->inv.ac_in_a == 0 && s->inv.ac_out_a == 5, "AACin/AACout");
    CHECK(NEAR(s->inv.freq_hz, 60.0), "Hz");
    CHECK(s->mult == 4, "48 V multiplier");

    /* REMOTE base values: the last remote packet (REMOTE_00's) */
    CHECK(NEAR(s->rem.revision, 4.0), "remote revision");
    CHECK(s->rem.search_w == 0 && s->rem.battery_type == 8 && NEAR(s->rem.absorb_v, 0),
          "searchwatts/battype/absorb");
    CHECK(s->rem.charge_rate == 100 && s->rem.ac_input_a == 10 && s->rem.parallel == 0,
          "chargeramps/ainput/parallel");
    CHECK(NEAR(s->rem.lbco_v, 20.0) && s->rem.ac_cutout_v == 155, "lbco/vaccutout");
    CHECK(NEAR(s->rem.float_v, 52.8) && NEAR(s->rem.eq_v, 1.2) &&
          NEAR(s->rem.absorb_time_h, 2.0), "vsfloat/vEQ/absorbtime");
    CHECK(s->rem.have_80 && s->rem.battery_size == 400, "batterysize (REMOTE_80)");
    CHECK(s->rem.have_clock && s->rem.clock_min == 18 * 60 + 32, "remote time 18:32");
    CHECK(s->rem.battery_efficiency_pct == 0, "batteryefficiency");
    /* REMOTE_A0 */
    CHECK(NEAR(s->rem.gen_run_time_h, 2.0), "A0 runtime (pymagnum: overwritten by A3)");
    CHECK(NEAR(s->rem.gen_start_temp_c, -17.8) && NEAR(s->rem.gen_start_v, 46.0) &&
          s->rem.quiet_time == 0, "starttemp/startvdc/quiettime");
    /* REMOTE_A1 */
    CHECK(s->rem.gen_start_time_min == 1440 && s->rem.gen_stop_time_min == 0,
          "begintime 2400 / stoptime 0");
    CHECK(NEAR(s->rem.gen_stop_v, 57.6) && s->rem.gen_volt_start_delay_s == 120 &&
          s->rem.gen_volt_stop_delay_s == 120 && NEAR(s->rem.gen_max_run_h, 12.0),
          "vdcstop/delays/maxrun");
    /* REMOTE_A2 */
    CHECK(s->rem.gen_soc_start_pct == 70 && s->rem.gen_soc_stop_pct == 85 &&
          s->rem.gen_amp_start_a == 0 && s->rem.gen_amp_start_delay_s == 120 &&
          s->rem.gen_amp_stop_a == 20 && s->rem.gen_amp_stop_delay_s == 120,
          "socstart/socstop/amps");
    /* REMOTE_A3 */
    CHECK(s->rem.quiet_begin_min == 23 * 60 + 30 && s->rem.quiet_end_min == 23 * 60 + 45 &&
          s->rem.exercise_start_min == 0, "quiet begin/end, exercisestart");
    CHECK(NEAR(s->rem.exercise_run_time_h, 3.6), "A3 byte 17 -> exercise run time");
    CHECK(s->rem.gen_topoff == 2, "topoff");
    /* REMOTE_A4 */
    CHECK(s->rem.gen_warmup_s == 30 && s->rem.gen_cooldown_s == 30, "warmup/cool");

    /* AGS */
    CHECK(NEAR(s->ags.revision, 5.2) && s->ags.status == 2 &&
          strcmp(mag_ags_status_text(2), "Ready") == 0 && !s->ags.running, "ags status");
    CHECK(s->ags.temp_valid && NEAR(s->ags.temp_c, 14.4), "ags temp");
    CHECK(NEAR(s->ags.run_time_h, 0.0) && NEAR(s->ags.battery_voltage_v, 50.8), "ags runtime/vdc");
    CHECK(s->ags.last_run == 0 && s->ags.last_full_soc == 0 && s->ags.total_run == 0, "ags A2");

    /* BMK */
    CHECK(NEAR(s->bmk.revision, 1.0) && s->bmk.soc_pct == 76, "bmk soc");
    CHECK(NEAR(s->bmk.battery_voltage_v, 25.45) && NEAR(s->bmk.battery_current_a, 11.6),
          "bmk vdc/adc");
    CHECK(NEAR(s->bmk.battery_voltage_min_v, 20.16) &&
          NEAR(s->bmk.battery_voltage_max_v, 30.8), "bmk vmin/vmax");
    CHECK(s->bmk.net_ah == -104 && NEAR(s->bmk.trip_ah, 2046.4) && s->bmk.lifetime_ah == 2000,
          "bmk amph/amphtrip/amphout");
    CHECK(s->bmk.fault == 1 && strcmp(mag_bmk_fault_text(1), "Normal") == 0, "bmk fault");

    /* RTR: pymagnum rounds to "3" */
    CHECK(NEAR(s->rtr.revision, 3.2), "router revision keeps its decimal");

    /* PT-100 */
    CHECK(s->pt.have_c1 && s->pt.address == 0 && s->pt.mode == 3 &&
          strcmp(mag_pt_mode_text(3), "Absorb") == 0, "pt mode");
    CHECK(s->pt.regulation == 1 && strcmp(mag_pt_regulation_text(1), "Voltage") == 0,
          "pt regulation");
    CHECK(s->pt.fault == 0 && strcmp(mag_pt_fault_text(0), "No Fault") == 0, "pt fault");
    CHECK(NEAR(s->pt.battery_voltage_v, 58.9) && NEAR(s->pt.battery_current_a, 13.3) &&
          NEAR(s->pt.pv_voltage_v, 146.8) && NEAR(s->pt.charge_time, 1.7),
          "pt battery/amps/pv/charge_time");
    CHECK(NEAR(s->pt.target_voltage_v, 59.2), "pt target (x4)");
    CHECK(!s->pt.relay_on && !s->pt.alarm_on && !s->pt.fan_on && s->pt.is_day, "pt bits");
    CHECK(s->pt.bat_temp_valid && s->pt.bat_temp_c == 27 && s->pt.inductor_temp_c == 49 &&
          s->pt.fet_temp_c == 62, "pt temps");
    CHECK(s->pt.have_c2 && s->pt.lifetime_kwh == 20 && NEAR(s->pt.resettable_kwh, 17.2) &&
          s->pt.ground_fault_current == 0 && s->pt.stacker_info == 0 &&
          NEAR(s->pt.nominal_voltage_v, 48.0) && s->pt.dip_switches == 0x05 &&
          NEAR(s->pt.revision, 1.1) && s->pt.output_current_rating_a == 100 &&
          s->pt.input_voltage_rating_v == 200, "pt C2");
}

static void test_sample_decode(void)
{
    stream_t s = { .n = 0 };
    rig_t r;

    sample_network(&s);
    rig_init(&r);
    feed(&r, &s, 0);
    check_sample_values(&r);
    CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 9 && r.f.st.packets[MAG_PKT_REMOTE] == 9,
          "9 inverter + 9 remote packets");
    CHECK(r.f.st.packets[MAG_PKT_AGS_A1] == 1 && r.f.st.packets[MAG_PKT_AGS_A2] == 1 &&
          r.f.st.packets[MAG_PKT_BMK_81] == 1 && r.f.st.packets[MAG_PKT_RTR_91] == 1 &&
          r.f.st.packets[MAG_PKT_PT_C1] == 1 && r.f.st.packets[MAG_PKT_PT_C2] == 1,
          "one of each peripheral reply");
    CHECK(r.f.st.bad_packets == 0 && r.f.st.unknown_bytes == 0 && r.f.st.resyncs == 0,
          "clean stream: nothing bad or unknown");
}

/* Framing must not depend on how the bytes arrive. */
static void test_chunking(void)
{
    static const size_t chunks[] = { 1, 2, 3, 7, 13, 21, 64, 250 };
    stream_t s = { .n = 0 };
    size_t i;

    sample_network(&s);
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
    {
        rig_t r;
        char msg[64];

        rig_init(&r);
        feed(&r, &s, chunks[i]);
        snprintf(msg, sizeof(msg), "chunks of %zu: same packets", chunks[i]);
        CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 9 &&
              r.f.st.packets[MAG_PKT_REMOTE] == 9 &&
              r.f.st.packets[MAG_PKT_PT_C2] == 1 && r.f.st.unknown_bytes == 0, msg);
        check_sample_values(&r);
    }
}

/* Garbage before, between and inside cycles: skipped, counted, resynced. */
static void test_garbage_and_resync(void)
{
    static const uint8_t junk[] = { 0xFF, 0xFF, 0x13, 0x00, 0x91, 0xA1, 0x55, 0xFF, 0x00 };
    stream_t s = { .n = 0 };
    rig_t r;
    uint8_t broken[21];
    size_t i;

    put_bytes(&s, junk, sizeof(junk));          /* start mid-stream */
    put(&s, REM_A0);                            /* a remote with no inverter */
    put(&s, INV48); put(&s, REM_A0); put(&s, AGS_A1);
    put_bytes(&s, junk, sizeof(junk));          /* noise between cycles */
    put(&s, INV48); put(&s, REM_80); put(&s, BMK_81);
    /* a damaged inverter packet: byte 20 not 0x00 */
    for (i = 0; i < 21; i++)
        sscanf(INV48 + 2 * i, "%2hhx", &broken[i]);
    broken[20] = 0x42;
    put_bytes(&s, broken, 21);
    put(&s, REM_00);
    put(&s, INV48); put(&s, REM_00); put(&s, RTR_91);

    rig_init(&r);
    feed(&r, &s, 5);
    CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 3, "three good inverter packets");
    CHECK(r.f.st.packets[MAG_PKT_AGS_A1] == 1 && r.f.st.packets[MAG_PKT_BMK_81] == 1 &&
          r.f.st.packets[MAG_PKT_RTR_91] == 1, "replies after resync");
    CHECK(r.f.st.unknown_bytes > 0 && r.f.st.resyncs >= 2, "garbage counted, resynced");
    CHECK(r.s.inv.seen && NEAR(r.s.inv.dc_voltage_v, 52.6), "values intact");
}

/* A cycle cut short, then silence: the tail is settled as bad. */
static void test_truncated_tail(void)
{
    stream_t s = { .n = 0 };
    rig_t r;

    put(&s, INV48); put(&s, REM_80);
    put(&s, "814C09F1007407");                  /* 7 of BMK_81's 18 bytes */
    rig_init(&r);
    mag_frame_feed(&r.f, s.b, s.n, 1.0);
    mag_frame_idle(&r.f, 1.01);                 /* not idle yet */
    CHECK(r.f.st.bad_packets == 0 && r.f.len == 7, "waits for the rest");
    mag_frame_idle(&r.f, 1.2);
    CHECK(r.f.st.bad_packets == 1 && r.f.len == 0, "settled as a bad packet");
    CHECK(!r.s.bmk.seen, "not decoded");
    /* The next cycle is clean. */
    s.n = 0;
    put(&s, INV48); put(&s, REM_80); put(&s, BMK_81);
    mag_frame_feed(&r.f, s.b, s.n, 1.3);
    CHECK(r.s.bmk.seen && r.s.bmk.soc_pct == 76, "next cycle decodes");
}

/* Inverter in Standby (mode 0): pymagnum files it as REMOTE_00. */
static void test_standby_inverter(void)
{
    stream_t s = { .n = 0 };
    rig_t r;

    put(&s, INV48); put(&s, REM_00);
    put(&s, INV48_STANDBY); put(&s, REM_00);
    rig_init(&r);
    feed(&r, &s, 0);
    CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 2, "standby packet is an inverter packet");
    CHECK(r.s.inv.mode == 0 && strcmp(mag_mode_text(0), "Standby") == 0, "mode Standby");
}

/* A cycle with no remote packet: inverter, then the next inverter. */
static void test_silent_remote(void)
{
    stream_t s = { .n = 0 };
    rig_t r;

    put(&s, INV48); put(&s, INV48); put(&s, REM_A0); put(&s, AGS_A1);
    rig_init(&r);
    feed(&r, &s, 0);
    CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 2 && r.f.st.packets[MAG_PKT_REMOTE] == 1 &&
          r.f.st.packets[MAG_PKT_AGS_A1] == 1 && r.f.st.bad_packets == 0,
          "inverter, inverter, remote, reply");
}

/* A tap that sees two inverters: follow the first, count the other. */
static void test_two_inverters(void)
{
    stream_t s = { .n = 0 };
    rig_t r;
    int i;

    for (i = 0; i < 5; i++)
    {
        put(&s, INV48); put(&s, REM_00);
        put(&s, INV48_SLAVE); put(&s, REM_00);
    }
    rig_init(&r);
    feed(&r, &s, 0);
    CHECK(r.s.inv.stackmode == 1, "follows the master");
    CHECK(r.s.other_inverter_packets == 5, "counts the slave's packets");
}

/* 0xFF-heavy input (crossed A/B): no packets, counted. */
static void test_polarity_noise(void)
{
    stream_t s = { .n = 0 };
    rig_t r;
    size_t i;

    for (i = 0; i < 500; i++)
        s.b[s.n++] = (i % 7) ? 0xFF : (uint8_t)(i & 0x7F);
    rig_init(&r);
    feed(&r, &s, 50);
    CHECK(r.f.st.packets[MAG_PKT_INVERTER] == 0, "no inverter packets from noise");
    CHECK(r.f.st.ff_bytes > 400 && r.f.st.unknown_bytes == 500, "all unknown, mostly 0xFF");
}

/* ---- the reading ---------------------------------------------------- */

static void test_reading_json(void)
{
    stream_t s = { .n = 0 };
    rig_t r;
    char *j;
    cJSON *o, *rem, *ags, *diag;

    sample_network(&s);
    put(&s, INV48); put(&s, REM_00); put(&s, "D100000000000000");  /* ACLD_D1 */
    rig_init(&r);
    feed(&r, &s, 0);
    j = mag_reading_json(&r.s, &r.f.st, "Inverter 1", "/dev/ttyUSB9", 3.0);
    CHECK(j != NULL, "reading built");
    CHECK(j && strlen(j) < 4096, "under 4 KB with every device present");
    o = j ? cJSON_Parse(j) : NULL;
    CHECK(o != NULL, "valid JSON");
    CHECK(o && strcmp(cJSON_GetObjectItem(o, "tap")->valuestring, "Inverter 1") == 0, "tap");
    CHECK(o && NEAR(cJSON_GetObjectItem(o, "dc_voltage_v")->valuedouble, 52.6), "dc_voltage_v");
    CHECK(o && NEAR(cJSON_GetObjectItem(o, "dc_power_w")->valuedouble, 1157.2), "dc_power_w");
    CHECK(o && NEAR(cJSON_GetObjectItem(o, "rated_w")->valuedouble, 4400), "rated_w");
    CHECK(o && NEAR(cJSON_GetObjectItem(o, "freq_hz")->valuedouble, 60.0), "freq_hz");
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItem(o, "invled")), "invled bool");
    CHECK(o && strcmp(cJSON_GetObjectItem(o, "stackmode_text")->valuestring,
                      "Parallel stack - master") == 0, "stackmode_text");
    rem = o ? cJSON_GetObjectItem(o, "remote") : NULL;
    CHECK(rem && strcmp(cJSON_GetObjectItem(rem, "clock_time")->valuestring, "18:32") == 0,
          "clock_time");
    CHECK(rem && strcmp(cJSON_GetObjectItem(rem, "gen_start_time")->valuestring, "24:00") == 0,
          "gen_start_time");
    CHECK(rem && strcmp(cJSON_GetObjectItem(rem, "quiet_begin_time")->valuestring, "23:30") == 0,
          "quiet_begin_time");
    CHECK(rem && cJSON_GetObjectItem(rem, "battery_efficiency_pct"), "BMK field (BMK seen)");
    ags = o ? cJSON_GetObjectItem(o, "ags") : NULL;
    CHECK(ags && NEAR(cJSON_GetObjectItem(ags, "temp_c")->valuedouble, 14.4), "ags temp_c");
    CHECK(o && cJSON_GetObjectItem(o, "router") && cJSON_GetObjectItem(o, "bmk") &&
          cJSON_GetObjectItem(o, "pt100") && cJSON_GetObjectItem(o, "acld"), "sub-objects");
    diag = o ? cJSON_GetObjectItem(o, "diag") : NULL;
    CHECK(diag && cJSON_GetObjectItem(diag, "packets") &&
          strcmp(cJSON_GetObjectItem(diag, "port")->valuestring, "/dev/ttyUSB9") == 0, "diag");
    cJSON_Delete(o);
    free(j);

    /* Before any peripheral: no sub-objects, remote-less AGS fields absent. */
    rig_init(&r);
    s.n = 0;
    put(&s, INV48); put(&s, REM_A0);
    feed(&r, &s, 0);
    j = mag_reading_json(&r.s, &r.f.st, NULL, NULL, 2.0);
    o = j ? cJSON_Parse(j) : NULL;
    CHECK(o && cJSON_IsNull(cJSON_GetObjectItem(o, "tap")), "no tap: null");
    CHECK(o && !cJSON_GetObjectItem(o, "ags") && !cJSON_GetObjectItem(o, "bmk"),
          "unseen devices absent");
    rem = o ? cJSON_GetObjectItem(o, "remote") : NULL;
    CHECK(rem && !cJSON_GetObjectItem(rem, "gen_run_time_h"),
          "AGS settings hidden until an AGS answers");
    CHECK(rem && cJSON_IsNull(cJSON_GetObjectItem(rem, "battery_size")), "battery_size null");
    cJSON_Delete(o);
    free(j);
}

static void test_names(void)
{
    uint8_t p[21] = { 0 };

    p[20] = 0xA3;
    CHECK(strcmp(mag_pkt_name(MAG_PKT_REMOTE, p, 21), "REMOTE_A3") == 0, "REMOTE_A3 name");
    CHECK(strcmp(mag_pkt_name(MAG_PKT_INVERTER, p, 21), "INVERTER") == 0, "INVERTER name");
    CHECK(mag_reply_len(0x81) == 18 && mag_reply_len(0x91) == 2 && mag_reply_len(0x40) == 0,
          "reply lengths");
    CHECK(strcmp(mag_fault_text(0x55), "Unknown") == 0, "unknown fault code");
    CHECK(mag_multiplier(0x0F) == 1 && mag_multiplier(0x6B) == 2 && mag_multiplier(0x73) == 4,
          "multipliers");
    /* Ratings from the model names: watts in hundreds, then volts. */
    CHECK(mag_model_rated_w(0x73) == 4400, "MS4448PAE: 4400 W");
    CHECK(mag_model_rated_w(0x06) == 600, "MM612: 600 W");
    CHECK(mag_model_rated_w(0x35) == 1300, "MM1324E: 1300 W");
    CHECK(mag_model_rated_w(0x2C) == 3000, "MSH3012M: 3000 W");
    CHECK(mag_model_rated_w(0x70) == 3700, "MS3748AEJ: 3700 W");
    CHECK(mag_model_rated_w(0xEE) == 0, "unknown model: no rating");
}

/* Pre-framed packets (magtest lines), identified one at a time. */
static void test_classify(void)
{
    static const struct { const char *hex; mag_pkt_t want; } k[] = {
        { "A102343A007F", MAG_PKT_AGS_A1 },
        { "4000020E0016780001003D11332473010005025800", MAG_PKT_INVERTER },
        { "00002808640A280000C89B840C1412200000000000", MAG_PKT_REMOTE },
        { "4000020E0016780001003D1133247301000502580000", MAG_PKT_INVERTER },
        { "C1001300024D008505BC1194101B313E", MAG_PKT_PT_C1 },
        { "9120", MAG_PKT_RTR_91 },
        { "FFFFFF", MAG_PKT_UNKNOWN },
    };
    mag_frame_t f;
    size_t i;

    mag_frame_init(&f, NULL, NULL);
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    {
        stream_t s = { .n = 0 };
        size_t len;
        char msg[80];

        put(&s, k[i].hex);
        len = s.n;
        snprintf(msg, sizeof(msg), "classify %s", k[i].hex);
        CHECK(mag_frame_classify(&f, s.b, &len) == k[i].want, msg);
    }
}

int main(void)
{
    test_classify();
    test_sample_decode();
    test_chunking();
    test_garbage_and_resync();
    test_truncated_tail();
    test_standby_inverter();
    test_silent_remote();
    test_two_inverters();
    test_polarity_noise();
    test_reading_json();
    test_names();
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_magnum: ok\n");
    return 0;
}
