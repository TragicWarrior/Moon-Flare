#ifndef MF_MAG_JSON_H
#define MF_MAG_JSON_H

/*
 * A Magnum tap's reading as JSON: the inverter's fields at the top level;
 * "remote", "router", "ags", "bmk", "pt100" and "acld" once their packets
 * have been seen, each with "last_seen_s"; and "diag".
 *
 * Built from pymagnum's per-device data (the magnum/...device.py classes,
 * getDevice()), with moon-flare field names.
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 */

#include "mag_decode.h"
#include "mag_frame.h"

/* Readings above this are trimmed (pt100, then bmk, then remote dropped). */
#define MAG_READING_MAX 4000

/* A malloc'd JSON object.  tap (the module's label) and port may be NULL. */
char *mag_reading_json(const mag_state_t *s, const mag_frame_stats_t *st,
                       const char *tap, const char *port, double now);

#endif
