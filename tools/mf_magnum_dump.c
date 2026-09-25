/*
 * mf_magnum_dump: watch a Magnum Energy RS-485 network, like pymagnum's
 * magtest: one "Length:NN TYPE =>HEX" line per packet, then a summary per
 * port of what was detected.  Read-only; it will not open a port that
 * moonflared (or another reader) holds, nor a /dev/ttyUSBn path.
 *
 * Derived from pymagnum magnum/magtest.py (main: the packet lines and the
 * detected-device summary).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 *
 *   mf_magnum_dump [-n PACKETS] [-t SECONDS] [--raw] [--json]
 *                  [--capture FILE] PORT...
 *   mf_magnum_dump [--json] --replay FILE
 *
 * --capture writes each read as "<seconds> <hex>" (one port), which
 * --replay (and moon-flare's tests) can play back with its timing.
 * --replay also takes pymagnum's "Length:NN TYPE =>HEX" packet files.
 */

#include "mag_decode.h"
#include "mag_frame.h"
#include "mag_json.h"
#include "mag_tty.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define MAX_PORTS 8

typedef struct {
    const char *path;
    const char *label;
    int         fd;
    int         done;
    int         packets;
    int         unknown;
    mag_frame_t fr;
    mag_state_t st;
} port_t;

static int    g_show_label;
static int    g_want = 50;              /* magtest's default --packets */
static double g_start;

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void hex(FILE *f, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        fprintf(f, "%02X", p[i]);
}

static void on_packet(void *arg, mag_pkt_t type, const uint8_t *p, size_t len,
                      double t)
{
    port_t *pt = arg;

    (void)mag_decode(&pt->st, type, p, len, t);
    if (pt->done)
        return;
    if (g_show_label)
        printf("[%s] ", pt->label);
    /* magtest: "Length:{0:2} {1:10}=>{2}" */
    printf("Length:%2zu %-10s=>", len, mag_pkt_name(type, p, len));
    hex(stdout, p, len);
    printf("\n");
    pt->packets++;
    if (type == MAG_PKT_UNKNOWN)
        pt->unknown++;
    if (g_want > 0 && pt->packets >= g_want)
        pt->done = 1;
}

static void summary(port_t *pt, double secs, int json, double now)
{
    const mag_frame_stats_t *st = &pt->fr.st;
    static const struct {
        const char *name;
        mag_pkt_t   a, b, c;
    } dev[] = {
        { "INVERTER", MAG_PKT_INVERTER, MAG_PKT_INVERTER, MAG_PKT_INVERTER },
        { "REMOTE",   MAG_PKT_REMOTE,   MAG_PKT_REMOTE,   MAG_PKT_REMOTE },
        { "RTR",      MAG_PKT_RTR_91,   MAG_PKT_RTR_91,   MAG_PKT_RTR_91 },
        { "AGS",      MAG_PKT_AGS_A1,   MAG_PKT_AGS_A2,   MAG_PKT_AGS_A2 },
        { "BMK",      MAG_PKT_BMK_81,   MAG_PKT_BMK_81,   MAG_PKT_BMK_81 },
        { "PT100",    MAG_PKT_PT_C1,    MAG_PKT_PT_C2,    MAG_PKT_PT_C3 },
        { "ACLD",     MAG_PKT_ACLD_D1,  MAG_PKT_ACLD_D1,  MAG_PKT_ACLD_D1 },
    };
    size_t i;

    printf("%s: Packets:%d with %d UNKNOWN, in %.2f seconds\n", pt->label,
           pt->packets, pt->unknown, secs);
    for (i = 0; i < sizeof(dev) / sizeof(dev[0]); i++)
    {
        uint64_t n = st->packets[dev[i].a];

        if (dev[i].b != dev[i].a)
            n += st->packets[dev[i].b];
        if (dev[i].c != dev[i].b)
            n += st->packets[dev[i].c];
        printf("  %-9s %s\n", dev[i].name, n ? "Detected" : "not detected");
    }
    if (pt->st.inv.seen)
        printf("  inverter: %s, %s\n", mag_model_text(pt->st.inv.model),
               mag_stackmode_text(pt->st.inv.stackmode));
    printf("  bytes %llu, bad packets %llu, unknown bytes %llu, resyncs %llu, "
           "0xFF %llu%s\n",
           (unsigned long long)st->bytes_read,
           (unsigned long long)st->bad_packets,
           (unsigned long long)st->unknown_bytes,
           (unsigned long long)st->resyncs,
           (unsigned long long)st->ff_bytes,
           st->bytes_read && st->ff_bytes * 2 > st->bytes_read
               ? " (mostly 0xFF: A/B crossed?)" : "");
    if (pt->st.other_inverter_packets)
        printf("  packets from another inverter: %llu (this tap sees more "
               "than one)\n", (unsigned long long)pt->st.other_inverter_packets);
    if (json)
    {
        char *j = mag_reading_json(&pt->st, st, NULL, pt->path, now);

        printf("%s\n", j ? j : "{}");
        free(j);
    }
}

static int from_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;

    while (s[0] && s[1] && n < cap)
    {
        unsigned v;

        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (int)n;
}

/* A capture ("<seconds> <hex>") or pymagnum packets ("Length:NN T =>HEX"). */
static int replay(const char *file, int json)
{
    FILE *f = fopen(file, "r");
    char line[8192];
    uint8_t buf[4096];
    port_t pt;
    double t = 0.0, t0 = -1.0;

    if (!f)
    {
        fprintf(stderr, "mf_magnum_dump: %s: %s\n", file, strerror(errno));
        return 1;
    }
    memset(&pt, 0, sizeof(pt));
    pt.path = file;
    pt.label = file;
    pt.fd = -1;
    g_want = 0;
    mag_frame_init(&pt.fr, on_packet, &pt);
    mag_state_init(&pt.st);
    while (fgets(line, sizeof(line), f))
    {
        char *arrow = strstr(line, "=>");
        char *hash = strchr(line, '#');
        double ts;
        int n;

        if (hash && (!arrow || hash < arrow))
            continue;
        if (arrow)
        {
            /* Already one packet per line: identify it, don't re-frame. */
            size_t len;
            mag_pkt_t type;

            n = from_hex(arrow + 2, buf, sizeof(buf));
            t += 0.010;
            if (t0 < 0)
                t0 = t;
            if (n <= 0)
                continue;
            len = (size_t)n;
            type = mag_frame_classify(&pt.fr, buf, &len);
            pt.fr.st.bytes_read += (size_t)n;
            if (type == MAG_PKT_UNKNOWN)
                pt.fr.st.unknown_bytes += (size_t)n;
            else
                pt.fr.st.packets[type]++;
            on_packet(&pt, type, buf, type == MAG_PKT_UNKNOWN ? (size_t)n : len, t);
            continue;
        }
        else
        {
            char hexs[8192];

            if (sscanf(line, "%lf %8191s", &ts, hexs) != 2)
                continue;
            n = from_hex(hexs, buf, sizeof(buf));
            t = ts;
        }
        if (t0 < 0)
            t0 = t;
        if (n > 0)
        {
            mag_frame_idle(&pt.fr, t);
            mag_frame_feed(&pt.fr, buf, (size_t)n, t);
        }
    }
    fclose(f);
    mag_frame_idle(&pt.fr, t + 1.0);
    summary(&pt, t0 < 0 ? 0 : t - t0, json, t);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: mf_magnum_dump [-n PACKETS] [-t SECONDS] [--raw] [--json]\n"
        "                      [--capture FILE] PORT...\n"
        "       mf_magnum_dump [--json] --replay FILE\n"
        "Read-only view of a Magnum Energy RS-485 network (like pymagnum's\n"
        "magtest).  -n: stop after this many packets per port (default 50,\n"
        "0 = no limit); -t: stop after this long (default 30 s).\n");
}

int main(int argc, char **argv)
{
    port_t ports[MAX_PORTS];
    int nports = 0, i, raw = 0, json = 0, live = 0;
    double limit_s = 30.0;
    const char *capture = NULL, *replay_file = NULL;
    FILE *cap = NULL;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            g_want = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            limit_s = atof(argv[++i]);
        else if (strcmp(argv[i], "--raw") == 0)
            raw = 1;
        else if (strcmp(argv[i], "--json") == 0)
            json = 1;
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc)
            capture = argv[++i];
        else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replay_file = argv[++i];
        else if (argv[i][0] == '-')
        {
            usage();
            return 2;
        }
        else if (nports < MAX_PORTS)
        {
            ports[nports].path = argv[i];
            nports++;
        }
    }
    if (replay_file)
        return replay(replay_file, json);
    if (nports == 0 || (capture && nports != 1))
    {
        usage();
        return 2;
    }
    g_show_label = nports > 1;
    g_start = now_mono();
    for (i = 0; i < nports; i++)
    {
        port_t *pt = &ports[i];
        char err[256];
        const char *slash = strrchr(pt->path, '/');

        pt->label = slash ? slash + 1 : pt->path;
        pt->done = 1;
        pt->fd = -1;
        /* ttyUSBn numbering shifts; the XD BMS owns ttyUSB0 and opening it
         * would retune its line.  Same rule as the plugin. */
        if (mag_tty_path_unstable(pt->path))
        {
            fprintf(stderr, "%s: use its /dev/serial/by-id path, not ttyUSBn\n",
                    pt->path);
            continue;
        }
        pt->done = 0;
        pt->packets = pt->unknown = 0;
        mag_frame_init(&pt->fr, on_packet, pt);
        mag_state_init(&pt->st);
        pt->fd = mag_tty_open(pt->path, err, sizeof(err));
        if (pt->fd < 0)
        {
            fprintf(stderr, "%s\n", err);
            pt->done = 1;
            continue;
        }
        live++;
        printf("Testing:%s\n", pt->path);
    }
    if (!live)
        return 1;
    if (capture)
    {
        cap = fopen(capture, "w");
        if (!cap)
        {
            fprintf(stderr, "mf_magnum_dump: %s: %s\n", capture, strerror(errno));
            return 1;
        }
        fprintf(cap, "# mf_magnum_dump capture of %s: <seconds> <hex>\n",
                ports[0].path);
    }
    for (;;)
    {
        fd_set r;
        struct timeval tv = { 0, 50000 };
        int maxfd = -1, active = 0;
        double now = now_mono();

        FD_ZERO(&r);
        for (i = 0; i < nports; i++)
        {
            if (ports[i].fd < 0 || ports[i].done)
                continue;
            FD_SET(ports[i].fd, &r);
            if (ports[i].fd > maxfd)
                maxfd = ports[i].fd;
            active++;
        }
        if (!active || now - g_start >= limit_s)
            break;
        if (select(maxfd + 1, &r, NULL, NULL, &tv) < 0 && errno != EINTR)
            break;
        now = now_mono();
        for (i = 0; i < nports; i++)
        {
            port_t *pt = &ports[i];
            uint8_t buf[1024];
            ssize_t n;

            if (pt->fd < 0 || pt->done)
                continue;
            while ((n = read(pt->fd, buf, sizeof(buf))) > 0)
            {
                if (raw)
                {
                    printf("  +%.6f %zd bytes: ", now - g_start, n);
                    hex(stdout, buf, (size_t)n);
                    printf("\n");
                }
                if (cap)
                {
                    fprintf(cap, "%.6f ", now - g_start);
                    hex(cap, buf, (size_t)n);
                    fprintf(cap, "\n");
                }
                mag_frame_feed(&pt->fr, buf, (size_t)n, now);
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            {
                fprintf(stderr, "%s: adapter disconnected\n", pt->path);
                pt->done = 1;
            }
            mag_frame_idle(&pt->fr, now);
        }
    }
    if (cap)
        fclose(cap);
    for (i = 0; i < nports; i++)
    {
        if (ports[i].fd < 0)
            continue;
        mag_frame_idle(&ports[i].fr, now_mono() + 1.0);
        summary(&ports[i], now_mono() - g_start, json, now_mono());
        mag_tty_close(ports[i].fd);
    }
    return 0;
}
