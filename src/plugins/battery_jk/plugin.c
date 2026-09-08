/*
 * libmf_battery_jk.so — JK02_32S over mf_gatt JSON-lines.
 * Handshake ACK before notify. Per-ctx assembler. No live BlueZ.
 */

#include "jk_proto.h"
#include "mf_plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define POLL_DEFAULT 1.0
#define POLL_MIN     1.0
#define LINE_CAP     4096
#define HEX_CAP      128

enum {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_HANDSHAKE,
    ST_STREAM
};

typedef struct {
    char     mac[32];
    char     adapter[16];
    char     name[64];
    char     gatt_bin[256];
    double   poll_interval_s;

    int      fd;
    unsigned select_mask;
    int      state;
    int      handshake_ok;
    double   next_try;

    char     in[LINE_CAP];
    size_t   in_len;
    char     out[LINE_CAP];
    size_t   out_len, out_off;

    jk_frame_assembler_t asm;
    uint8_t              frame[JK_FRAME_SIZE];
    jk_cell_info_t       cell;
    int                  have_data;
    int                  have_trigger;
    int                  have_start;
    int                  need_cell_req;
    double               last_cell_mono;
    double               trigger_v;
    double               start_v;
    char                 err[96];
} jk_ctx_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

enum { H_NONE = 0, H_STARTING, H_UP, H_DEAD, HMAX = 4 };

typedef struct {
    char  adapter[16];
    int   st;
    pid_t pid;
    int   refcnt;
} helper_row_t;

static helper_row_t g_help[HMAX];

static helper_row_t *helper_row(const char *adapter)
{
    int i, empty = -1;
    const char *ad = adapter && adapter[0] ? adapter : "hci0";
    for (i = 0; i < HMAX; i++) {
        if (g_help[i].adapter[0] && strcmp(g_help[i].adapter, ad) == 0)
            return &g_help[i];
        if (empty < 0 && !g_help[i].adapter[0])
            empty = i;
    }
    if (empty < 0)
        return NULL;
    snprintf(g_help[empty].adapter, sizeof(g_help[empty].adapter), "%s", ad);
    g_help[empty].st = H_NONE;
    g_help[empty].pid = -1;
    g_help[empty].refcnt = 0;
    return &g_help[empty];
}

static int helper_needed(int e)
{
    /* Abstract @mf-gatt/<adapter> is not in the filesystem: a missing
     * listener is ECONNREFUSED. Pathname sockets may return ENOENT. */
    return e == ENOENT || e == ECONNREFUSED;
}

static int helper_spawn(jk_ctx_t *c)
{
    helper_row_t *h = helper_row(c->adapter);
    const char *bin = c->gatt_bin[0] ? c->gatt_bin : getenv("MF_GATT_BIN");
    pid_t pid;
    if (!h)
        return -1;
    if (h->st == H_STARTING) {
        if (h->pid > 0 && kill(h->pid, 0) != 0 && errno == ESRCH)
            h->st = H_NONE;
        else
            return 0;
    }
    if (h->st == H_UP)
        return 0;
    if (!bin || !bin[0])
        bin = "/usr/local/libexec/mf_gatt";
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl(bin, bin, "--adapter", h->adapter, (char *)NULL);
        _exit(127);
    }
    h->pid = pid;
    h->st = H_STARTING;
    return 0;
}

static void helper_up(jk_ctx_t *c)
{
    helper_row_t *h = helper_row(c->adapter);
    if (!h)
        return;
    h->st = H_UP;
    h->refcnt++;
}

static void helper_dead(jk_ctx_t *c)
{
    helper_row_t *h = helper_row(c->adapter);
    if (!h)
        return;
    if (h->refcnt > 0)
        h->st = H_DEAD;
}

static void helper_release(jk_ctx_t *c)
{
    helper_row_t *h = helper_row(c->adapter);
    if (!h || h->refcnt <= 0)
        return;
    h->refcnt--;
    if (h->refcnt == 0 && h->st == H_UP)
        h->st = H_NONE;
}

static const char *json_find_key(const char *js, const char *key)
{
    char pat[64];
    if (!js || !key)
        return NULL;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(js, pat);
}

static void json_str(const char *js, const char *key, char *dst, size_t cap)
{
    const char *p, *q;
    size_t n;
    dst[0] = '\0';
    p = json_find_key(js, key);
    if (!p)
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
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static double json_double(const char *js, const char *key, double def)
{
    const char *p = json_find_key(js, key);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    return atof(p + 1);
}

static const char *json_ble_scope(const char *js)
{
    const char *p = json_find_key(js, "ble");
    return p ? p : (js ? js : "");
}

static void set_err(jk_ctx_t *c, const char *m)
{
    snprintf(c->err, sizeof(c->err), "%s", m ? m : "");
}

static void sock_close(jk_ctx_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->select_mask = 0;
    c->state = ST_IDLE;
    c->handshake_ok = 0;
    c->in_len = c->out_len = c->out_off = 0;
    c->next_try = mono_now() + 0.5;
}

static int abs_addr(struct sockaddr_un *a, socklen_t *alen, const char *adapter)
{
    int n;
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    a->sun_path[0] = '\0';
    n = snprintf(a->sun_path + 1, sizeof(a->sun_path) - 1, "mf-gatt/%s",
                 adapter && adapter[0] ? adapter : "hci0");
    if (n < 0 || (size_t)n >= sizeof(a->sun_path) - 1)
        return -1;
    *alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    return 0;
}

static int start_connect(jk_ctx_t *c)
{
    struct sockaddr_un a;
    socklen_t alen;
    int fd, rc;
    if (abs_addr(&a, &alen, c->adapter) != 0) {
        set_err(c, "bad adapter");
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_err(c, "socket");
        return -1;
    }
    rc = connect(fd, (struct sockaddr *)&a, alen);
    if (rc != 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        if (helper_needed(e)) {
            (void)helper_spawn(c);
            set_err(c, "helper starting");
            return -1;
        }
        snprintf(c->err, sizeof(c->err), "connect: %s", strerror(e));
        return -1;
    }
    c->fd = fd;
    c->state = (rc == 0) ? ST_HANDSHAKE : ST_CONNECTING;
    c->select_mask = (rc == 0) ? MF_IO_WANT_WRITE : MF_IO_WANT_WRITE;
    c->handshake_ok = 0;
    return 0;
}

static void queue_str(jk_ctx_t *c, const char *s)
{
    size_t n = strlen(s);
    if (c->out_len + n + 1 >= sizeof(c->out))
        return;
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
    c->out[c->out_len++] = '\n';
    c->select_mask |= MF_IO_WANT_WRITE;
}

static void send_connect(jk_ctx_t *c)
{
    char line[128];
    snprintf(line, sizeof(line), "{\"cmd\":\"connect\",\"address\":\"%s\"}",
             c->mac);
    queue_str(c, line);
}

static int queue_hex_write(jk_ctx_t *c, const uint8_t *cmd);

static int hex_nibble(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int hex_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (in && in[0] && in[1] && n < cap) {
        int hi = hex_nibble((unsigned char)in[0]);
        int lo = hex_nibble((unsigned char)in[1]);
        if (hi < 0 || lo < 0)
            break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        in += 2;
    }
    return (int)n;
}

static void hex_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char *d = "0123456789abcdef";
    size_t i;
    if (cap < n * 2 + 1)
        n = (cap - 1) / 2;
    for (i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static void on_line(jk_ctx_t *c, const char *line)
{
    char type[24], cmd[24], hex[HEX_CAP];
    type[0] = cmd[0] = hex[0] = '\0';
    json_str(line, "type", type, sizeof(type));
    json_str(line, "cmd", cmd, sizeof(cmd));
    json_str(line, "hex", hex, sizeof(hex));
    if (strcmp(type, "ok") == 0 && strcmp(cmd, "connect") == 0) {
        if (!c->handshake_ok)
            helper_up(c);
        c->handshake_ok = 1;
        c->need_cell_req = 1;
        c->state = ST_STREAM;
        c->err[0] = '\0';
        return;
    }
    if (strcmp(type, "error") == 0) {
        json_str(line, "message", c->err, sizeof(c->err));
        return;
    }
    if (strcmp(type, "notify") == 0 && c->handshake_ok && hex[0]) {
        uint8_t raw[64];
        int n = hex_decode(hex, raw, sizeof(raw));
        if (n > 0 &&
            jk_assembler_feed(&c->asm, raw, (size_t)n, c->frame, 1) > 0) {
            if (jk_decode_cell_info(c->frame, JK_FRAME_SIZE, JK_PROTO_JK02_32S,
                                    0, &c->cell) == JK_OK) {
                c->have_data = 1;
                c->last_cell_mono = mono_now();
            }
        }
    }
}

static void drain_in(jk_ctx_t *c)
{
    for (;;) {
        ssize_t n;
        char *nl;
        if (c->in_len + 1 >= sizeof(c->in))
            c->in_len = 0;
        n = read(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            sock_close(c);
            set_err(c, "read");
            return;
        }
        if (n == 0) {
            if (c->handshake_ok)
                helper_dead(c);
            sock_close(c);
            set_err(c, "helper closed");
            return;
        }
        c->in_len += (size_t)n;
        c->in[c->in_len] = '\0';
        while ((nl = memchr(c->in, '\n', c->in_len)) != NULL) {
            size_t ln = (size_t)(nl - c->in);
            c->in[ln] = '\0';
            if (ln && c->in[ln - 1] == '\r')
                c->in[ln - 1] = '\0';
            on_line(c, c->in);
            memmove(c->in, nl + 1, c->in_len - ln - 1);
            c->in_len -= ln + 1;
            c->in[c->in_len] = '\0';
        }
    }
}

static void flush_out(jk_ctx_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            sock_close(c);
            return;
        }
        c->out_off += (size_t)n;
    }
    c->out_len = c->out_off = 0;
}

static mf_step_t pump(jk_ctx_t *c)
{
    double now = mono_now();
    if (c->state == ST_IDLE) {
        if (now < c->next_try)
            return MF_STEP_IDLE;
        if (start_connect(c) != 0)
            return MF_STEP_IDLE;
        if (c->state == ST_HANDSHAKE)
            send_connect(c);
    }
    if (c->state == ST_CONNECTING) {
        struct pollfd p;
        int err = 0;
        socklen_t el = (socklen_t)sizeof(err);
        p.fd = c->fd;
        p.events = POLLOUT;
        p.revents = 0;
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLOUT)) {
            c->select_mask = MF_IO_WANT_WRITE;
            return MF_STEP_IDLE;
        }
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            int e = err ? err : errno;
            sock_close(c);
            if (helper_needed(e))
                (void)helper_spawn(c);
            set_err(c, "connect failed");
            return MF_STEP_ERROR;
        }
        c->state = ST_HANDSHAKE;
        send_connect(c);
    }
    if (c->fd >= 0)
        drain_in(c);
    if (c->handshake_ok && c->fd >= 0) {
        if (c->need_cell_req) {
            queue_hex_write(c, jk_build_command(JK_CMD_DEVICE_INFO, NULL, 0, 0));
            queue_hex_write(c, jk_build_command(JK_CMD_CELL_INFO, NULL, 0, 0));
            c->need_cell_req = 0;
            c->last_cell_mono = now;
        } else if (now - c->last_cell_mono > 8.0) {
            queue_hex_write(c, jk_build_command(JK_CMD_CELL_INFO, NULL, 0, 0));
            c->last_cell_mono = now;
        }
    }
    if (c->fd >= 0)
        flush_out(c);
    c->select_mask = 0;
    if (c->fd < 0)
        return MF_STEP_ERROR;
    c->select_mask = MF_IO_WANT_READ;
    if (c->out_len > c->out_off)
        c->select_mask |= MF_IO_WANT_WRITE;
    if (c->state == ST_CONNECTING)
        c->select_mask = MF_IO_WANT_WRITE;
    if (c->have_data)
        return MF_STEP_UPDATED;
    return MF_STEP_IDLE;
}

static void *jk_open(const char *spec_json, char *err, size_t errsz)
{
    jk_ctx_t *c = calloc(1, sizeof(*c));
    const char *ble;
    double iv;
    if (!c) {
        if (err && errsz)
            snprintf(err, errsz, "oom");
        return NULL;
    }
    c->fd = -1;
    c->poll_interval_s = POLL_DEFAULT;
    snprintf(c->adapter, sizeof(c->adapter), "hci0");
    ble = json_ble_scope(spec_json ? spec_json : "");
    json_str(ble, "address", c->mac, sizeof(c->mac));
    json_str(ble, "adapter", c->adapter, sizeof(c->adapter));
    json_str(spec_json ? spec_json : "", "name", c->name, sizeof(c->name));
    json_str(spec_json ? spec_json : "", "gatt_bin", c->gatt_bin,
             sizeof(c->gatt_bin));
    if (!c->adapter[0])
        snprintf(c->adapter, sizeof(c->adapter), "hci0");
    iv = json_double(spec_json ? spec_json : "", "poll_interval_s", 0.0);
    if (iv > 0.0)
        c->poll_interval_s = iv;
    if (c->poll_interval_s < POLL_MIN)
        c->poll_interval_s = POLL_MIN;
    jk_assembler_init(&c->asm);
    if (!c->mac[0]) {
        set_err(c, "ble.address required");
        if (err && errsz)
            snprintf(err, errsz, "%s", c->err);
        /* still return ctx; step will ERROR */
        return c;
    }
    if (start_connect(c) != 0) {
        if (err && errsz)
            snprintf(err, errsz, "%s", c->err);
        c->next_try = mono_now() + 0.2;
    } else if (c->state == ST_HANDSHAKE) {
        send_connect(c);
    }
    return c;
}

static void jk_close(void *v)
{
    jk_ctx_t *c = v;
    helper_row_t *h;
    if (!c)
        return;
    h = helper_row(c->adapter);
    if (c->fd >= 0 && c->handshake_ok) {
        if (h && h->refcnt <= 1)
            queue_str(c, "{\"cmd\":\"quit\"}");
        else
            queue_str(c, "{\"cmd\":\"disconnect\"}");
        flush_out(c);
    }
    if (c->handshake_ok)
        helper_release(c);
    sock_close(c);
    free(c);
}

static int jk_fd(void *v)
{
    jk_ctx_t *c = v;
    return c ? c->fd : -1;
}

static unsigned jk_mask(void *v)
{
    jk_ctx_t *c = v;
    return c ? c->select_mask : 0;
}

static mf_step_t jk_step(void *v)
{
    jk_ctx_t *c = v;
    if (!c)
        return MF_STEP_ERROR;
    return pump(c);
}

static unsigned jk_caps(void *v)
{
    (void)v;
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS | MF_CAP_ACTION_SWITCH |
           MF_CAP_ACTION_REFRESH | MF_CAP_PROBE;
}

static const char *jk_last_error(void *v)
{
    jk_ctx_t *c = v;
    return (c && c->err[0]) ? c->err : "";
}

static int js_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*off >= cap)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= cap - *off) {
        *off = cap - 1;
        buf[cap - 1] = '\0';
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

static int jk_get_reading(void *v, char *json, size_t cap)
{
    jk_ctx_t *c = v;
    const jk_cell_info_t *r;
    size_t off = 0;
    int i;
    if (!c || !json || cap == 0)
        return -1;
    if (!c->have_data) {
        snprintf(json, cap, "{}");
        return 0;
    }
    r = &c->cell;
    js_append(json, cap, &off,
              "{\"pack_voltage_v\":%.3f,\"current_a\":%.2f,\"soc_pct\":%.1f,"
              "\"soh_pct\":%.1f,\"cell_count\":%u,"
              "\"charge_mosfet_on\":%s,\"discharge_mosfet_on\":%s,"
              "\"balancer_switch\":%s,\"cells\":[",
              (double)r->pack_voltage_v, (double)r->current_a,
              (double)r->soc_pct, (double)r->soh_pct, (unsigned)r->cell_count,
              r->charge_mosfet_on ? "true" : "false",
              r->discharge_mosfet_on ? "true" : "false",
              r->balancing_indicator ? "true" : "false");
    for (i = 0; i < (int)r->cell_count; i++)
        js_append(json, cap, &off, "%s{\"index\":%d,\"voltage_v\":%.3f}",
                  i ? "," : "", i + 1, (double)r->cells[i].voltage_v);
    js_append(json, cap, &off, "]}");
    return 0;
}

static int jk_get_settings(void *v, char *json, size_t cap)
{
    jk_ctx_t *c = v;
    if (!c || !json || cap == 0)
        return -1;
    snprintf(json, cap,
             "{\"ble.address\":\"%s\",\"ble.adapter\":\"%s\","
             "\"poll_interval_s\":%.1f",
             c->mac, c->adapter, c->poll_interval_s);
    if (c->have_trigger) {
        size_t n = strlen(json);
        snprintf(json + n, cap - n, ",\"balance_trigger_v\":%.3f", c->trigger_v);
    }
    if (c->have_start) {
        size_t n = strlen(json);
        snprintf(json + n, cap - n, ",\"start_balance_v\":%.3f", c->start_v);
    }
    {
        size_t n = strlen(json);
        if (n + 2 <= cap) {
            json[n] = '}';
            json[n + 1] = '\0';
        }
    }
    return 0;
}

static int jk_put_settings(void *v, const char *json, char *err, size_t errsz)
{
    jk_ctx_t *c = v;
    double iv;
    if (err && errsz)
        err[0] = '\0';
    if (!c)
        return MF_ERR_INVAL;
    iv = json_double(json ? json : "", "poll_interval_s", -1.0);
    if (iv >= 0.0) {
        if (iv < POLL_MIN) {
            if (err && errsz)
                snprintf(err, errsz, "poll_interval_s below %.1f", POLL_MIN);
            return MF_ERR_INVAL;
        }
        c->poll_interval_s = iv;
    }
    return MF_OK;
}

static int queue_hex_write(jk_ctx_t *c, const uint8_t *cmd)
{
    char hex[48], line[192];
    hex_encode(cmd, JK_CMD_FRAME_SIZE, hex, sizeof(hex));
    snprintf(line, sizeof(line),
             "{\"cmd\":\"write\",\"address\":\"%s\",\"hex\":\"%s\","
             "\"response\":false}",
             c->mac, hex);
    queue_str(c, line);
    return MF_OK;
}

static int jk_action(void *v, const char *action, const char *json,
                     char *err, size_t errsz)
{
    jk_ctx_t *c = v;
    uint8_t reg = 0;
    int on = 1;
    const uint8_t *cmd;
    double volts;
    uint32_t mv;
    if (err && errsz)
        err[0] = '\0';
    if (!c || !action)
        return MF_ERR_INVAL;
    if (!c->handshake_ok) {
        if (err && errsz)
            snprintf(err, errsz, "offline");
        return MF_ERR_OFFLINE;
    }
    if (strcmp(action, "refresh") == 0) {
        cmd = jk_build_command(JK_CMD_CELL_INFO, NULL, 0, 0);
        return queue_hex_write(c, cmd);
    }
    if (strcmp(action, "set_balance_trigger") == 0) {
        if (!json || !json_find_key(json, "volts")) {
            if (err && errsz)
                snprintf(err, errsz, "volts required");
            return MF_ERR_INVAL;
        }
        volts = jk_clamp_trigger_v(json_double(json, "volts", 0.0));
        mv = jk_volts_to_mv(volts);
        c->trigger_v = volts;
        c->have_trigger = 1;
        cmd = jk_build_register_cmd(JK_REG_BALANCE_TRIGGER, mv);
        return queue_hex_write(c, cmd);
    }
    if (strcmp(action, "set_start_balance") == 0) {
        if (!json || !json_find_key(json, "volts")) {
            if (err && errsz)
                snprintf(err, errsz, "volts required");
            return MF_ERR_INVAL;
        }
        volts = jk_clamp_start_v(json_double(json, "volts", 0.0));
        mv = jk_volts_to_mv(volts);
        c->start_v = volts;
        c->have_start = 1;
        cmd = jk_build_register_cmd(JK_REG_START_BALANCE_JK02_32S, mv);
        return queue_hex_write(c, cmd);
    }
    if (strcmp(action, "set_switch") != 0) {
        if (err && errsz)
            snprintf(err, errsz, "unsupported");
        return MF_ERR_UNSUPPORTED;
    }
    if (json && strstr(json, "false"))
        on = 0;
    if (json && strstr(json, "discharge"))
        reg = JK_REG_DISCHARGE;
    else if (json && strstr(json, "balance"))
        reg = JK_REG_BALANCE;
    else
        reg = JK_REG_CHARGE;
    cmd = jk_build_switch_cmd(reg, on != 0);
    return queue_hex_write(c, cmd);
}

static int jk_probe_start(const char *args_json, void **job, char *err, size_t errsz)
{
    (void)args_json;
    (void)job;
    if (err && errsz)
        snprintf(err, errsz, "ble probe later");
    return -1;
}

static mf_step_t jk_probe_step(void *job)
{
    (void)job;
    return MF_STEP_ERROR;
}

static unsigned jk_probe_mask(void *job)
{
    (void)job;
    return 0;
}

static void jk_probe_prepare(void *job, fd_set *r, fd_set *w, int *maxfd)
{
    (void)job;
    (void)r;
    (void)w;
    (void)maxfd;
}

static int jk_probe_result(void *job, char *json, size_t cap)
{
    (void)job;
    if (json && cap)
        snprintf(json, cap, "{\"status\":\"error\",\"results\":[]}");
    return -1;
}

static void jk_probe_close(void *job)
{
    (void)job;
}

static const mf_plugin_ops_t g_ops = {
    .abi = MF_PLUGIN_ABI,
    .ops_size = sizeof(mf_plugin_ops_t),
    .kind = "battery",
    .driver = "jk",
    .version = "0.1.0",
    .open = jk_open,
    .close = jk_close,
    .fd = jk_fd,
    .select_mask = jk_mask,
    .prepare_fds = NULL,
    .step = jk_step,
    .caps = jk_caps,
    .last_error = jk_last_error,
    .get_reading = jk_get_reading,
    .get_settings = jk_get_settings,
    .put_settings = jk_put_settings,
    .action = jk_action,
    .probe_start = jk_probe_start,
    .probe_step = jk_probe_step,
    .probe_select_mask = jk_probe_mask,
    .probe_prepare_fds = jk_probe_prepare,
    .probe_result = jk_probe_result,
    .probe_close = jk_probe_close,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = &g_ops;
    return 1;
}
