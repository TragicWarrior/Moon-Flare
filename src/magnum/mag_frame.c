/*
 * Magnum RS-485 framing.  See mag_frame.h for the design.
 *
 * Derived from pymagnum magnum/magnum.py (Magnum.readPackets,
 * Magnum._parsePacket, Magnum.cleanup).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 *
 * Packet identification follows _parsePacket: lengths, first bytes of the
 * peripheral replies, the remote's type byte (byte 20), and an inverter
 * packet told apart from a REMOTE_00 by byte 20 == 0x00 plus the model and
 * revision learned from the first inverter packet.  pymagnum's precedence
 * bug in that test (Standby inverter packets become REMOTE_00) is not
 * reproduced.  The 22->21 / 17->16 trimming and cleanup()'s merging of
 * split packets repair timing-based framing and are not needed here.
 */

#include "mag_frame.h"
#include "mag_decode.h"

#include <string.h>

enum {
    ST_HUNT = 0,            /* looking for an inverter packet */
    ST_AFTER_INV,           /* the remote packet comes next */
    ST_AFTER_REMOTE         /* an optional peripheral reply comes next */
};

#define CHANGED ((size_t)-1)    /* state changed, nothing consumed */

static int be16s(const uint8_t *p)
{
    return (int16_t)(uint16_t)((p[0] << 8) | p[1]);
}

size_t mag_reply_len(uint8_t b)
{
    switch (b)
    {
    case 0x91: return 2;    /* RTR_91 */
    case 0xA1: return 6;    /* AGS_A1 */
    case 0xA2: return 6;    /* AGS_A2 */
    case 0xD1: return 8;    /* ACLD_D1 */
    case 0xC2: return 13;   /* PT_C2 */
    case 0xC3: return 14;   /* PT_C3 */
    case 0xC1: return 16;   /* PT_C1 */
    case 0x81: return 18;   /* BMK_81 */
    }
    return 0;
}

static mag_pkt_t reply_type(uint8_t b)
{
    switch (b)
    {
    case 0x91: return MAG_PKT_RTR_91;
    case 0xA1: return MAG_PKT_AGS_A1;
    case 0xA2: return MAG_PKT_AGS_A2;
    case 0xD1: return MAG_PKT_ACLD_D1;
    case 0xC2: return MAG_PKT_PT_C2;
    case 0xC3: return MAG_PKT_PT_C3;
    case 0xC1: return MAG_PKT_PT_C1;
    case 0x81: return MAG_PKT_BMK_81;
    }
    return MAG_PKT_UNKNOWN;
}

/* The remote's byte 20: which peripheral it is polling (_parsePacket). */
static int remote_type_ok(uint8_t b)
{
    switch (b)
    {
    case 0x00: case 0x11: case 0x80:
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4:
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xD0:
        return 1;
    }
    return 0;
}

int mag_frame_is_inverter(const mag_frame_t *f, const uint8_t *p)
{
    int freq, vdc, mult;

    if (p[20] != 0x00)
        return 0;
    if (f->have_id)
    {
        if (p[14] != f->inv_model || p[10] != f->inv_revision)
            return 0;
    }
    else if (!mag_model_known(p[14]))
        return 0;
    /* A remote packet whose last 7 bytes are zero is never an inverter's
     * (_parsePacket's "sevenzeros"): byte 14, the model, is among them. */
    freq = be16s(p + 18);
    if (freq != 0 && (freq < 400 || freq > 700))       /* 40-70 Hz */
        return 0;
    if (p[11] > 150 || p[12] > 150 || p[13] > 150)     /* temperatures */
        return 0;
    mult = mag_multiplier(p[14]);
    vdc = be16s(p + 2);                                 /* tenths of a volt */
    if (vdc < 0 || vdc > 120 * mult * 2)                /* 0 .. 2x nominal */
        return 0;
    return 1;
}

static int reply_ok(const uint8_t *p)
{
    switch (p[0])
    {
    case 0xA1:
        return (int8_t)p[1] >= 0 && (int8_t)p[1] <= 27;    /* AGS status */
    case 0x81:
        return (int8_t)p[1] >= 0 && (int8_t)p[1] <= 100;   /* BMK SOC */
    }
    return 1;
}

static void emit(mag_frame_t *f, mag_pkt_t t, const uint8_t *p, size_t len)
{
    if (t != MAG_PKT_UNKNOWN)
        f->st.packets[t]++;
    if (f->fn)
        f->fn(f->arg, t, p, len, f->last_rx);
}

static void skip(mag_frame_t *f, size_t n)
{
    if (!n)
        return;
    f->st.unknown_bytes += n;
    emit(f, MAG_PKT_UNKNOWN, f->buf, n);
    if (f->synced)
    {
        f->st.resyncs++;
        f->synced = 0;
    }
}

static size_t hunt(mag_frame_t *f)
{
    size_t i;

    if (f->len < MAG_INV_LEN)
        return 0;
    for (i = 0; i + MAG_INV_LEN <= f->len; i++)
        if (mag_frame_is_inverter(f, f->buf + i))
            break;
    if (i + MAG_INV_LEN > f->len)
    {
        /* None here; the last 20 bytes may be the start of one. */
        size_t drop = f->len - (MAG_INV_LEN - 1);

        skip(f, drop);
        return drop;
    }
    if (i > 0)
    {
        skip(f, i);
        return i;
    }
    if (!f->have_id)
    {
        f->have_id = 1;
        f->inv_model = f->buf[14];
        f->inv_revision = f->buf[10];
    }
    f->since_inv = 0;
    f->synced = 1;
    emit(f, MAG_PKT_INVERTER, f->buf, MAG_INV_LEN);
    f->state = ST_AFTER_INV;
    return MAG_INV_LEN;
}

static size_t after_inv(mag_frame_t *f)
{
    const uint8_t *p = f->buf;

    if (f->len < MAG_REMOTE_LEN)
        return 0;
    /* A byte-20 0x00 packet is a REMOTE_00 unless it is the next
     * inverter packet (the remote said nothing this cycle). */
    if (remote_type_ok(p[20]) && !(p[20] == 0x00 && mag_frame_is_inverter(f, p)))
    {
        emit(f, MAG_PKT_REMOTE, p, MAG_REMOTE_LEN);
        f->state = ST_AFTER_REMOTE;
        return MAG_REMOTE_LEN;
    }
    if (mag_frame_is_inverter(f, p))
    {
        f->state = ST_HUNT;             /* hunt() takes it at offset 0 */
        return CHANGED;
    }
    f->st.bad_packets++;
    f->state = ST_HUNT;
    return CHANGED;
}

static size_t after_remote(mag_frame_t *f)
{
    size_t n;

    if (f->len < 1)
        return 0;
    n = mag_reply_len(f->buf[0]);
    if (!n)
    {
        /* No reply this cycle: the next inverter packet should follow. */
        f->state = ST_HUNT;
        return CHANGED;
    }
    if (f->len < n)
        return 0;                       /* mag_frame_idle() settles it */
    f->state = ST_HUNT;
    if (!reply_ok(f->buf))
    {
        f->st.bad_packets++;
        return CHANGED;
    }
    emit(f, reply_type(f->buf[0]), f->buf, n);
    return n;
}

static void consume(mag_frame_t *f, size_t n)
{
    if (n >= f->len)
    {
        f->len = 0;
        return;
    }
    memmove(f->buf, f->buf + n, f->len - n);
    f->len -= n;
}

static void process(mag_frame_t *f)
{
    for (;;)
    {
        size_t used = 0;

        switch (f->state)
        {
        case ST_HUNT:         used = hunt(f); break;
        case ST_AFTER_INV:    used = after_inv(f); break;
        case ST_AFTER_REMOTE: used = after_remote(f); break;
        }
        if (used == CHANGED)
            continue;
        if (!used)
            return;
        consume(f, used);
    }
}

void mag_frame_init(mag_frame_t *f, mag_packet_fn fn, void *arg)
{
    memset(f, 0, sizeof(*f));
    f->fn = fn;
    f->arg = arg;
}

void mag_frame_reset_stream(mag_frame_t *f)
{
    f->len = 0;
    f->state = ST_HUNT;
    f->synced = 0;
    f->since_inv = 0;
}

void mag_frame_reset_stats(mag_frame_t *f)
{
    memset(&f->st, 0, sizeof(f->st));
}

void mag_frame_feed(mag_frame_t *f, const uint8_t *data, size_t n, double now)
{
    size_t i;

    if (!n)
        return;
    f->last_rx = now;
    f->st.bytes_read += n;
    for (i = 0; i < n; i++)
        if (data[i] == 0xFF)
            f->st.ff_bytes++;
    f->since_inv += n;
    if (f->have_id && f->since_inv > MAG_RELEARN_BYTES)
        f->have_id = 0;
    while (n)
    {
        size_t room = sizeof(f->buf) - f->len;
        size_t take = n < room ? n : room;

        memcpy(f->buf + f->len, data, take);
        f->len += take;
        data += take;
        n -= take;
        process(f);
    }
}

void mag_frame_idle(mag_frame_t *f, double now)
{
    if (!f->len || now - f->last_rx < MAG_IDLE_S)
        return;
    /* The bus went quiet with bytes still waiting. */
    if (f->state != ST_HUNT)
        f->st.bad_packets++;            /* a packet cut short */
    f->st.unknown_bytes += f->len;
    emit(f, MAG_PKT_UNKNOWN, f->buf, f->len);
    f->len = 0;
    f->state = ST_HUNT;
}

mag_pkt_t mag_frame_classify(mag_frame_t *f, const uint8_t *p, size_t *len)
{
    size_t n = *len;

    if (n == 22)
        n = 21;
    else if (n == 17)
        n = 16;
    *len = n;
    if (n == MAG_INV_LEN)
    {
        if (mag_frame_is_inverter(f, p))
        {
            if (!f->have_id)
            {
                f->have_id = 1;
                f->inv_model = p[14];
                f->inv_revision = p[10];
            }
            return MAG_PKT_INVERTER;
        }
        return remote_type_ok(p[20]) ? MAG_PKT_REMOTE : MAG_PKT_UNKNOWN;
    }
    if (n && mag_reply_len(p[0]) == n)
        return reply_type(p[0]);
    return MAG_PKT_UNKNOWN;
}

const char *mag_pkt_name(mag_pkt_t type, const uint8_t *p, size_t len)
{
    static const char *const remote[] = {
        "REMOTE_00", "REMOTE_11", "REMOTE_80", "REMOTE_A0", "REMOTE_A1",
        "REMOTE_A2", "REMOTE_A3", "REMOTE_A4", "REMOTE_C0", "REMOTE_C1",
        "REMOTE_C2", "REMOTE_C3", "REMOTE_D0"
    };
    static const uint8_t remote_b[] = {
        0x00, 0x11, 0x80, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xC0, 0xC1, 0xC2,
        0xC3, 0xD0
    };
    size_t i;

    switch (type)
    {
    case MAG_PKT_INVERTER: return "INVERTER";
    case MAG_PKT_AGS_A1:   return "AGS_A1";
    case MAG_PKT_AGS_A2:   return "AGS_A2";
    case MAG_PKT_BMK_81:   return "BMK_81";
    case MAG_PKT_RTR_91:   return "RTR_91";
    case MAG_PKT_PT_C1:    return "PT_C1";
    case MAG_PKT_PT_C2:    return "PT_C2";
    case MAG_PKT_PT_C3:    return "PT_C3";
    case MAG_PKT_ACLD_D1:  return "ACLD_D1";
    case MAG_PKT_REMOTE:
        for (i = 0; p && len >= MAG_REMOTE_LEN && i < sizeof(remote_b); i++)
            if (p[20] == remote_b[i])
                return remote[i];
        return "REMOTE";
    default:
        break;
    }
    return "UNKNOWN";
}
