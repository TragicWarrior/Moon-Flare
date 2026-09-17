/*
 * Classic autodetection probe job. Last-known ip:port first; only then a
 * bounded TCP sweep (≤8 in-flight). UDP advertisement is a phase-1 stub.
 * Persist is probe_result JSON; this file never fsyncs.
 */

#include "classic_codec.h"
#include "mf_plugin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REG_START          4100
#define REG_QTY            113
#define REG_IMAGE          4220
#define IO_TIMEOUT_S       1.5
#define PROBE_CONNECT_S    0.2
#define PROBE_CAP_S        15.0
#define PROBE_INFLIGHT     8
#define PROBE_MAX_CAND     512
#define PROBE_MAX_SKIP     32
#define PROBE_MAX_HIT      8
#define RX_CAP             512
#define TX_CAP             16

enum {
    SL_FREE = 0,
    SL_CONNECTING,
    SL_SEND,
    SL_RECV
};

enum {
    PJ_LASTKNOWN = 0,
    PJ_SCAN,
    PJ_DONE
};

typedef struct {
    char     ip[64];
    int      port;
    int      unit_try[3];
    int      ntry;
    int      itry;
    int      fd;
    unsigned mask;
    int      state;
    double   deadline;
    uint8_t  tx[TX_CAP];
    size_t   tx_len, tx_off;
    uint8_t  rx[RX_CAP];
    size_t   rx_len;
    uint16_t tid;
    uint16_t regs[REG_IMAGE];
} probe_slot_t;

typedef struct {
    char     ip[64];
    int      port;
    int      unit;
    char     mac[24];
    uint32_t devid;
} probe_hit_t;

typedef struct {
    int      auto_net;
    char     last_ip[64];
    int      last_port;
    int      unit_cfg;
    char     want_mac[32];
    char     listen[80];

    char     skip[PROBE_MAX_SKIP][64];
    int      nskip;
    int      have_cand_key;

    char     cand_ip[PROBE_MAX_CAND][64];
    int      cand_port[PROBE_MAX_CAND];
    int      ncand;
    int      next_cand;

    probe_slot_t slots[PROBE_INFLIGHT];
    int      phase;
    double   t0;

    probe_hit_t hits[PROBE_MAX_HIT];
    int      nhit;
    char     err[96];
} probe_job_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static const char *json_find_key(const char *js, const char *key)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || !js)
        return NULL;
    return strstr(js, pat);
}

static int json_int(const char *js, const char *key, int def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    return atoi(p + 1);
}

static int json_bool(const char *js, const char *key, int def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == 't' || *p == 'T' || *p == '1')
        return 1;
    if (*p == 'f' || *p == 'F' || *p == '0')
        return 0;
    return def;
}

static void json_str(const char *js, const char *key, char *dst, size_t dstsz)
{
    const char *p = json_find_key(js, key);
    const char *q;
    size_t n;
    dst[0] = '\0';
    if (!p || dstsz == 0)
        return;
    p = strchr(p, ':');
    if (!p)
        return;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return;
    p++;
    q = strchr(p, '"');
    if (!q)
        return;
    n = (size_t)(q - p);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static const char *json_modbus_scope(const char *js)
{
    const char *p = json_find_key(js, "modbus");
    return p ? p : (js ? js : "");
}

static void fill_units(int cfg, int *u, int *n)
{
    int seen10 = 0, seen1 = 0;
    *n = 0;
    if (cfg > 0) {
        u[(*n)++] = cfg;
        if (cfg == 10)
            seen10 = 1;
        if (cfg == 1)
            seen1 = 1;
    }
    if (!seen10)
        u[(*n)++] = 10;
    if (!seen1)
        u[(*n)++] = 1;
}

static void mac_fmt(const uint8_t m[6], char *dst, size_t n)
{
    snprintf(dst, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int skipped(const probe_job_t *j, const char *ip)
{
    int i;
    for (i = 0; i < j->nskip; i++) {
        if (strcmp(j->skip[i], ip) == 0)
            return 1;
    }
    return 0;
}

static void add_cand(probe_job_t *j, const char *ip, int port)
{
    int i;
    struct in_addr a;
    if (!ip || !ip[0] || j->ncand >= PROBE_MAX_CAND)
        return;
    if (port <= 0 || port > 65535)
        return;
    if (inet_pton(AF_INET, ip, &a) != 1)
        return;
    if (skipped(j, ip))
        return;
    if (j->last_ip[0] && strcmp(j->last_ip, ip) == 0 && port == j->last_port)
        return;
    for (i = 0; i < j->ncand; i++) {
        if (strcmp(j->cand_ip[i], ip) == 0 && j->cand_port[i] == port)
            return;
    }
    snprintf(j->cand_ip[j->ncand], sizeof(j->cand_ip[0]), "%s", ip);
    j->cand_port[j->ncand] = port;
    j->ncand++;
}

static int parse_hostport(const char *s, size_t n, char *ip, size_t ipsz, int *port)
{
    char buf[80];
    char *colon;
    if (n == 0 || n >= sizeof(buf))
        return -1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    colon = strrchr(buf, ':');
    if (!colon || colon == buf)
        return -1;
    *colon = '\0';
    if (snprintf(ip, ipsz, "%s", buf) >= (int)ipsz)
        return -1;   /* host longer than caller's buffer */
    *port = atoi(colon + 1);
    return 0;
}

static void parse_string_array(const char *js, const char *key,
                               void (*got)(void *, const char *, size_t),
                               void *ctx)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return;
    p = strchr(p, '[');
    if (!p)
        return;
    p++;
    while (*p && *p != ']') {
        const char *q;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',' || *p == '\r')
            p++;
        if (*p != '"')
            break;
        p++;
        q = strchr(p, '"');
        if (!q)
            break;
        got(ctx, p, (size_t)(q - p));
        p = q + 1;
    }
}

static void on_candidate(void *ctx, const char *s, size_t n)
{
    probe_job_t *j = ctx;
    char ip[64];
    int port = 0;
    if (parse_hostport(s, n, ip, sizeof(ip), &port) == 0)
        add_cand(j, ip, port);
}

static void on_skip(void *ctx, const char *s, size_t n)
{
    probe_job_t *j = ctx;
    if (j->nskip >= PROBE_MAX_SKIP || n == 0)
        return;
    if (n >= sizeof(j->skip[0]))
        n = sizeof(j->skip[0]) - 1;
    memcpy(j->skip[j->nskip], s, n);
    j->skip[j->nskip][n] = '\0';
    j->nskip++;
}

static int default_src_ipv4(char *out, size_t outsz)
{
    int fd;
    struct sockaddr_in a, l;
    socklen_t ln = (socklen_t)sizeof(l);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    (void)inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&l, &ln) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (!inet_ntop(AF_INET, &l.sin_addr, out, (socklen_t)outsz))
        return -1;
    return 0;
}

static int listen_host_ip(const char *listen, char *out, size_t outsz)
{
    char host[64];
    const char *colon;
    if (!listen || !listen[0])
        return default_src_ipv4(out, outsz);
    colon = strrchr(listen, ':');
    if (colon && colon != listen && (size_t)(colon - listen) < sizeof(host)) {
        memcpy(host, listen, (size_t)(colon - listen));
        host[colon - listen] = '\0';
        if (strcmp(host, "0.0.0.0") != 0 && strcmp(host, "*") != 0) {
            snprintf(out, outsz, "%s", host);
            return 0;
        }
    }
    return default_src_ipv4(out, outsz);
}

static void fill_subnet(probe_job_t *j)
{
    char base[64];
    unsigned a, b, c, d, host;
    int ports[2];
    int np = 0;

    if (listen_host_ip(j->listen, base, sizeof(base)) != 0)
        return;
    if (sscanf(base, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return;
    if (a > 255u || b > 255u || c > 255u || d > 255u)
        return;
    ports[np++] = 502;
    if (j->last_port > 0 && j->last_port != 502)
        ports[np++] = j->last_port;
    syslog(LOG_NOTICE,
           "moonflare: classic auto_net TCP probe %u.%u.%u.0/24 (%d port%s)",
           a, b, c, np, np == 1 ? "" : "s");
    for (host = 1; host <= 254; host++) {
        int pi;
        char ip[16];
        if (host == d)
            continue;
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, host);
        for (pi = 0; pi < np; pi++)
            add_cand(j, ip, ports[pi]);
    }
}

static void slot_close(probe_slot_t *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    s->mask = 0;
    s->state = SL_FREE;
    s->tx_len = s->tx_off = 0;
    s->rx_len = 0;
}

static void job_finish(probe_job_t *j)
{
    int i;
    j->phase = PJ_DONE;
    for (i = 0; i < PROBE_INFLIGHT; i++)
        slot_close(&j->slots[i]);
}

static int connect_ready(int fd)
{
    struct pollfd p;
    int err = 0;
    socklen_t elen = (socklen_t)sizeof(err);

    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLOUT))
        return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
        return -1;
    return 1;
}

static int slot_connect(probe_slot_t *s)
{
    struct sockaddr_in a;
    int one = 1;
    int fd;
    uint8_t unit;

    slot_close(s);
    if (s->itry < 0 || s->itry >= s->ntry)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)s->port);
    if (inet_pton(AF_INET, s->ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    s->fd = fd;
    s->state = SL_CONNECTING;
    s->mask = MF_IO_WANT_WRITE;
    s->deadline = mono_now() + PROBE_CONNECT_S;
    s->tx_len = s->tx_off = 0;
    s->rx_len = 0;
    s->tid++;
    if (s->tid == 0)
        s->tid = 1;
    unit = (uint8_t)s->unit_try[s->itry];
    put_be16(s->tx + 0, s->tid);
    put_be16(s->tx + 2, 0);
    put_be16(s->tx + 4, 6);
    s->tx[6] = unit;
    s->tx[7] = 0x03;
    put_be16(s->tx + 8, (uint16_t)REG_START);
    put_be16(s->tx + 10, (uint16_t)REG_QTY);
    s->tx_len = 12;
    return 0;
}

static int decode_rx(probe_slot_t *s, classic_data_t *data)
{
    uint16_t length, i;
    uint8_t fc, nbytes;
    classic_registers_t input;
    classic_result_t rc;

    if (s->rx_len < 6)
        return 0;
    length = be16(s->rx + 4);
    if (length < 3 || (size_t)length + 6 > sizeof(s->rx))
        return -1;
    if (s->rx_len < 6u + (size_t)length)
        return 0;
    fc = s->rx[7];
    if (fc & 0x80u)
        return -2;
    if (fc != 0x03)
        return -1;
    nbytes = s->rx[8];
    if (nbytes != (uint8_t)(REG_QTY * 2) || 6u + (size_t)length < 9u + nbytes)
        return -1;
    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < (uint16_t)REG_QTY; i++)
        s->regs[REG_START + i] = be16(s->rx + 9 + (size_t)i * 2u);
    input.regs = s->regs;
    input.reg_count = REG_IMAGE;
    rc = classic_decode(&input, data);
    if (rc == CLASSIC_ETYPE_MISMATCH)
        return -3;
    if (rc != CLASSIC_OK)
        return -1;
    return 1;
}

/* 1 = decoded, 0 = in flight, -1 = candidate dead (try next host). */
static int slot_pump(probe_slot_t *s, classic_data_t *data)
{
    double now = mono_now();

    if (s->state == SL_FREE)
        return -1;

    if (now > s->deadline) {
        s->itry++;
        if (s->itry >= s->ntry || slot_connect(s) != 0) {
            slot_close(s);
            return -1;
        }
        return 0;
    }

    if (s->state == SL_CONNECTING) {
        int cr = connect_ready(s->fd);
        if (cr == 0)
            return 0;
        if (cr < 0) {
            s->itry++;
            if (s->itry >= s->ntry || slot_connect(s) != 0) {
                slot_close(s);
                return -1;
            }
            return 0;
        }
        s->state = SL_SEND;
        s->mask = MF_IO_WANT_WRITE;
        s->deadline = now + IO_TIMEOUT_S;
        s->tx_off = 0;
    }

    if (s->state == SL_SEND) {
        while (s->tx_off < s->tx_len) {
            ssize_t n = send(s->fd, s->tx + s->tx_off, s->tx_len - s->tx_off,
                             MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    return 0;
                s->itry++;
                if (s->itry >= s->ntry || slot_connect(s) != 0) {
                    slot_close(s);
                    return -1;
                }
                return 0;
            }
            if (n == 0) {
                s->itry++;
                if (s->itry >= s->ntry || slot_connect(s) != 0) {
                    slot_close(s);
                    return -1;
                }
                return 0;
            }
            s->tx_off += (size_t)n;
        }
        s->state = SL_RECV;
        s->mask = MF_IO_WANT_READ;
        s->rx_len = 0;
        s->deadline = now + IO_TIMEOUT_S;
    }

    if (s->state == SL_RECV) {
        for (;;) {
            ssize_t n;
            int pr;
            if (s->rx_len >= sizeof(s->rx)) {
                slot_close(s);
                return -1;
            }
            n = recv(s->fd, s->rx + s->rx_len, sizeof(s->rx) - s->rx_len, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    return 0;
                s->itry++;
                if (s->itry >= s->ntry || slot_connect(s) != 0) {
                    slot_close(s);
                    return -1;
                }
                return 0;
            }
            if (n == 0) {
                s->itry++;
                if (s->itry >= s->ntry || slot_connect(s) != 0) {
                    slot_close(s);
                    return -1;
                }
                return 0;
            }
            s->rx_len += (size_t)n;
            pr = decode_rx(s, data);
            if (pr == 0)
                continue;
            if (pr == 1) {
                slot_close(s);
                return 1;
            }
            if (pr == -3) {
                /* 4209 mismatch: not a Classic. Do not try other unit ids. */
                slot_close(s);
                return -1;
            }
            /* exception or bad PDU: next unit */
            s->itry++;
            if (s->itry >= s->ntry || slot_connect(s) != 0) {
                slot_close(s);
                return -1;
            }
            return 0;
        }
    }

    return 0;
}

static int mac_ok(const probe_job_t *j, const classic_data_t *d)
{
    char mac[24];
    if (!j->want_mac[0])
        return 1;
    mac_fmt(d->unit_mac, mac, sizeof(mac));
    return strcasecmp(j->want_mac, mac) == 0;
}

static void add_hit(probe_job_t *j, const probe_slot_t *s, const classic_data_t *d)
{
    char mac[24];
    int i;
    if (j->nhit >= PROBE_MAX_HIT)
        return;
    mac_fmt(d->unit_mac, mac, sizeof(mac));
    for (i = 0; i < j->nhit; i++) {
        if (strcmp(j->hits[i].ip, s->ip) == 0 && j->hits[i].port == s->port)
            return;
    }
    snprintf(j->hits[j->nhit].ip, sizeof(j->hits[0].ip), "%s", s->ip);
    j->hits[j->nhit].port = s->port;
    j->hits[j->nhit].unit = (s->itry >= 0 && s->itry < s->ntry)
                            ? s->unit_try[s->itry] : 10;
    snprintf(j->hits[j->nhit].mac, sizeof(j->hits[0].mac), "%s", mac);
    j->hits[j->nhit].devid = d->device_id;
    j->nhit++;
}

static int inflight(const probe_job_t *j)
{
    int i, n = 0;
    for (i = 0; i < PROBE_INFLIGHT; i++) {
        if (j->slots[i].state != SL_FREE)
            n++;
    }
    return n;
}

static void start_lastknown(probe_job_t *j)
{
    probe_slot_t *s = &j->slots[0];
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    snprintf(s->ip, sizeof(s->ip), "%s", j->last_ip);
    s->port = j->last_port;
    fill_units(j->unit_cfg, s->unit_try, &s->ntry);
    s->itry = 0;
    if (slot_connect(s) != 0)
        slot_close(s);
}

static void fill_scan_slots(probe_job_t *j)
{
    int i;
    while (j->next_cand < j->ncand && inflight(j) < PROBE_INFLIGHT) {
        probe_slot_t *s = NULL;
        for (i = 0; i < PROBE_INFLIGHT; i++) {
            if (j->slots[i].state == SL_FREE) {
                s = &j->slots[i];
                break;
            }
        }
        if (!s)
            break;
        memset(s, 0, sizeof(*s));
        s->fd = -1;
        snprintf(s->ip, sizeof(s->ip), "%s", j->cand_ip[j->next_cand]);
        s->port = j->cand_port[j->next_cand];
        j->next_cand++;
        fill_units(j->unit_cfg, s->unit_try, &s->ntry);
        s->itry = 0;
        if (slot_connect(s) != 0)
            slot_close(s);
    }
}

static int consider_hit(probe_job_t *j, probe_slot_t *s, const classic_data_t *d)
{
    if (!mac_ok(j, d))
        return 0;
    add_hit(j, s, d);
    return 1;
}

int classic_probe_start(const char *args_json, void **job, char *err, size_t errsz)
{
    probe_job_t *j;
    const char *js = args_json ? args_json : "{}";
    const char *mb;
    int i;

    if (!job) {
        if (err && errsz)
            snprintf(err, errsz, "no job out");
        return MF_ERR_INVAL;
    }
    j = calloc(1, sizeof(*j));
    if (!j) {
        if (err && errsz)
            snprintf(err, errsz, "oom");
        return MF_ERR_INVAL;
    }
    for (i = 0; i < PROBE_INFLIGHT; i++)
        j->slots[i].fd = -1;

    mb = json_modbus_scope(js);
    json_str(mb, "ip", j->last_ip, sizeof(j->last_ip));
    j->last_port = json_int(mb, "port", 502);
    if (j->last_port <= 0 || j->last_port > 65535)
        j->last_port = 502;
    j->unit_cfg = json_int(mb, "unit_id", 10);
    json_str(mb, "mac", j->want_mac, sizeof(j->want_mac));
    j->auto_net = json_bool(js, "auto_net", json_bool(mb, "auto_net", 1));
    json_str(js, "listen", j->listen, sizeof(j->listen));

    parse_string_array(js, "skip", on_skip, j);
    j->have_cand_key = json_find_key(js, "candidates") != NULL;
    parse_string_array(js, "candidates", on_candidate, j);
    if (!j->have_cand_key && j->auto_net)
        fill_subnet(j);

    j->t0 = mono_now();
    if (j->last_ip[0]) {
        j->phase = PJ_LASTKNOWN;
        start_lastknown(j);
        if (j->slots[0].state == SL_FREE) {
            if (j->auto_net)
                j->phase = PJ_SCAN;
            else
                j->phase = PJ_DONE;
        }
    } else if (j->auto_net) {
        j->phase = PJ_SCAN;
    } else {
        j->phase = PJ_DONE;
    }
    if (j->phase == PJ_SCAN)
        fill_scan_slots(j);

    *job = j;
    if (err && errsz)
        err[0] = '\0';
    return MF_OK;
}

mf_step_t classic_probe_step(void *job)
{
    probe_job_t *j = job;
    classic_data_t data;
    int i;

    if (!j)
        return MF_STEP_ERROR;
    if (j->phase == PJ_DONE)
        return MF_STEP_IDLE;
    if (mono_now() - j->t0 >= PROBE_CAP_S) {
        job_finish(j);
        return MF_STEP_UPDATED;
    }

    if (j->phase == PJ_LASTKNOWN) {
        int pr = slot_pump(&j->slots[0], &data);
        if (pr == 1) {
            if (consider_hit(j, &j->slots[0], &data)) {
                job_finish(j);
                return MF_STEP_UPDATED;
            }
            /* type-ok but MAC mismatch: fall through to scan if allowed */
        }
        if (pr != 0) {
            if (!j->auto_net) {
                job_finish(j);
                return MF_STEP_UPDATED;
            }
            j->phase = PJ_SCAN;
            fill_scan_slots(j);
        } else {
            return MF_STEP_IDLE;
        }
    }

    if (j->phase == PJ_SCAN) {
        fill_scan_slots(j);
        for (i = 0; i < PROBE_INFLIGHT; i++) {
            int pr;
            if (j->slots[i].state == SL_FREE)
                continue;
            pr = slot_pump(&j->slots[i], &data);
            if (pr == 1 && consider_hit(j, &j->slots[i], &data)) {
                job_finish(j);
                return MF_STEP_UPDATED;
            }
        }
        fill_scan_slots(j);
        if (inflight(j) == 0 && j->next_cand >= j->ncand) {
            job_finish(j);
            return MF_STEP_UPDATED;
        }
    }

    return MF_STEP_IDLE;
}

unsigned classic_probe_select_mask(void *job)
{
    probe_job_t *j = job;
    unsigned m = 0;
    int i;
    if (!j)
        return 0;
    for (i = 0; i < PROBE_INFLIGHT; i++)
        m |= j->slots[i].mask;
    return m;
}

void classic_probe_prepare_fds(void *job, fd_set *r, fd_set *w, int *maxfd)
{
    probe_job_t *j = job;
    int i;
    if (!j || !maxfd)
        return;
    for (i = 0; i < PROBE_INFLIGHT; i++) {
        int fd = j->slots[i].fd;
        unsigned mask = j->slots[i].mask;
        if (fd < 0 || !mask)
            continue;
        if ((mask & MF_IO_WANT_READ) && r)
            FD_SET((unsigned)fd, r);
        if ((mask & MF_IO_WANT_WRITE) && w)
            FD_SET((unsigned)fd, w);
        if (fd > *maxfd)
            *maxfd = fd;
    }
}

int classic_probe_result(void *job, char *json, size_t cap)
{
    probe_job_t *j = job;
    size_t off = 0;
    int i;
    const char *st;
    int n;

    if (!j || !json || cap == 0)
        return -1;
    st = (j->phase == PJ_DONE) ? "done" : "running";
    n = snprintf(json, cap, "{\"status\":\"%s\",\"results\":[", st);
    if (n < 0)
        return -1;
    off = (size_t)n;
    for (i = 0; i < j->nhit && off < cap; i++) {
        n = snprintf(json + off, cap - off,
                     "%s{\"kind\":\"charger\",\"driver\":\"classic\","
                     "\"bus\":\"modbus\",\"endpoint\":\"%s:%d\","
                     "\"mac\":\"%s\",\"unit_id\":%d,\"unit_device_id\":%u,"
                     "\"in_use\":false}",
                     i ? "," : "",
                     j->hits[i].ip, j->hits[i].port, j->hits[i].mac,
                     j->hits[i].unit, (unsigned)j->hits[i].devid);
        if (n < 0)
            break;
        off += (size_t)n;
    }
    if (off < cap) {
        n = snprintf(json + off, cap - off, "]}");
        if (n < 0)
            json[cap - 1] = '\0';
    } else {
        json[cap - 1] = '\0';
    }
    return 0;
}

void classic_probe_close(void *job)
{
    probe_job_t *j = job;
    if (!j)
        return;
    job_finish(j);
    free(j);
}
