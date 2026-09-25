#ifndef MF_MAG_FRAME_H
#define MF_MAG_FRAME_H

/*
 * Magnum Energy RS-485 network: framing packets out of a passive tap.
 *
 * Derived from pymagnum magnum/magnum.py (Magnum.readPackets,
 * Magnum._parsePacket, Magnum.cleanup).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 *
 * pymagnum ends a packet when a read sees 5 ms without a byte.  That
 * timing does not survive a USB adapter's latency timer or a shared event
 * loop, so this framer works from content and the bus cycle instead: every
 * ~100 ms the inverter sends a 21-byte packet, the remote (or router)
 * answers with 21 bytes whose last byte names the peripheral it polls, and
 * at most one peripheral replies (fixed length by its first byte).
 *
 *   - Inverter packets are the anchor: 21 bytes ending 0x00, a known model,
 *     plausible frequency, temperatures and DC volts; once one is seen, the
 *     model and revision must match exactly (as in pymagnum).
 *   - The 21 bytes after an inverter packet are the remote packet (its type
 *     byte validated); then an optional reply; then the next inverter.
 *   - Anything that does not fit is skipped and counted, and the framer
 *     hunts for the next inverter packet (a resync).
 *
 * Timing is only used to settle an incomplete tail: when nothing has
 * arrived for MAG_IDLE_S, bytes still waiting are counted as bad or
 * unknown.  Parsing never splits received data by time, so a stalled
 * reader (bytes wait in the kernel's tty buffer) changes nothing.
 */

#include <stddef.h>
#include <stdint.h>

#define MAG_BUF_SIZE     4096
#define MAG_INV_LEN      21
#define MAG_REMOTE_LEN   21
#define MAG_IDLE_S       0.060
/* Relearn the inverter's model/revision after this many bytes without one
 * (a firmware update, or the tap moved to another inverter). */
#define MAG_RELEARN_BYTES 4096

typedef enum {
    MAG_PKT_UNKNOWN = 0,    /* bytes that are not part of a packet */
    MAG_PKT_INVERTER,
    MAG_PKT_REMOTE,         /* 21 bytes; the type is byte 20 */
    MAG_PKT_AGS_A1,
    MAG_PKT_AGS_A2,
    MAG_PKT_BMK_81,
    MAG_PKT_RTR_91,
    MAG_PKT_PT_C1,
    MAG_PKT_PT_C2,
    MAG_PKT_PT_C3,
    MAG_PKT_ACLD_D1,
    MAG_PKT_COUNT
} mag_pkt_t;

/* Called for each packet (and each run of skipped bytes) in stream order. */
typedef void (*mag_packet_fn)(void *arg, mag_pkt_t type, const uint8_t *p,
                              size_t len, double t);

typedef struct {
    uint64_t bytes_read;
    uint64_t packets[MAG_PKT_COUNT];    /* [MAG_PKT_UNKNOWN] unused */
    uint64_t bad_packets;               /* failed validation or cut short */
    uint64_t unknown_bytes;             /* skipped while hunting */
    uint64_t resyncs;                   /* lost the cycle and hunted */
    uint64_t ff_bytes;                  /* 0xFF bytes: crossed A/B shows many */
} mag_frame_stats_t;

typedef struct {
    uint8_t  buf[MAG_BUF_SIZE];
    size_t   len;
    int      state;
    int      synced;                    /* in step with the cycle */
    int      have_id;                   /* learned model/revision */
    uint8_t  inv_model;
    uint8_t  inv_revision;
    uint64_t since_inv;                 /* bytes since the last inverter */
    double   last_rx;
    mag_frame_stats_t st;
    mag_packet_fn fn;
    void    *arg;
} mag_frame_t;

void mag_frame_init(mag_frame_t *f, mag_packet_fn fn, void *arg);
/* Forget buffered bytes and cycle position (a reopened port); keep stats. */
void mag_frame_reset_stream(mag_frame_t *f);
void mag_frame_reset_stats(mag_frame_t *f);
void mag_frame_feed(mag_frame_t *f, const uint8_t *data, size_t n, double now);
/* Call after reading: settles an incomplete tail once the bus is idle. */
void mag_frame_idle(mag_frame_t *f, double now);

/* One packet already cut out of the stream (a pymagnum/magtest line):
 * identify it as _parsePacket does (22 and 17 bytes trimmed by one).  The
 * old 16-byte protocol is not supported.  *len is updated. */
mag_pkt_t mag_frame_classify(mag_frame_t *f, const uint8_t *p, size_t *len);

/* pymagnum's name for a packet: "INVERTER", "REMOTE_A0", "BMK_81", ... */
const char *mag_pkt_name(mag_pkt_t type, const uint8_t *p, size_t len);
/* Length of a peripheral reply starting with byte b, or 0. */
size_t mag_reply_len(uint8_t b);
/* Would p (21 bytes) pass as an inverter packet? */
int mag_frame_is_inverter(const mag_frame_t *f, const uint8_t *p);

#endif
