/*
 * Fake mf_gatt helper. Abstract Unix socket @mf-gatt/<adapter>.
 * Handshake ACK before notify. Two canned MACs with distinct voltages.
 * write hex of a JK switch command is isolated per MAC. No BlueZ.
 */

#include "jk_proto.h"

#include <errno.h>
#include <poll.h>
#include <strings.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAC_A "28:D4:1E:A7:23:39"
#define MAC_B "AA:BB:CC:DD:EE:FF"
#define MAX_CLI 8
#define LINE_CAP 4096
#define CHUNK 20

typedef struct {
    int      fd;
    char     mac[32];
    int      bound;
    int      acked;
    char     in[LINE_CAP];
    size_t   in_len;
    char     out[LINE_CAP];
    size_t   out_len, out_off;
    uint8_t  frame[JK_FRAME_SIZE];
    size_t   frame_off;
    int      charge, discharge, balance;
    int      cell_mv;
    uint32_t trigger_mv;
    uint32_t start_mv;
    uint32_t ovp_mv;
    uint32_t ovpr_mv;
    uint32_t rcv_mv;
    unsigned tick_wait;
} cli_t;

static cli_t g_cli[MAX_CLI];
static const char *g_adapter = "hci0";

static void put_u16(uint8_t *buf, size_t off, uint16_t val)
{
    buf[off] = (uint8_t)val;
    buf[off + 1] = (uint8_t)(val >> 8);
}

static void put_u32(uint8_t *buf, size_t off, uint32_t val)
{
    buf[off] = (uint8_t)val;
    buf[off + 1] = (uint8_t)(val >> 8);
    buf[off + 2] = (uint8_t)(val >> 16);
    buf[off + 3] = (uint8_t)(val >> 24);
}

static void put_i16(uint8_t *buf, size_t off, int16_t val)
{
    put_u16(buf, off, (uint16_t)val);
}

static void put_i32(uint8_t *buf, size_t off, int32_t val)
{
    put_u32(buf, off, (uint32_t)val);
}

static void build_frame(cli_t *c)
{
    uint8_t *buf = c->frame;
    int i, n = 16;
    uint16_t mv = (uint16_t)c->cell_mv;
    uint32_t pack = (uint32_t)mv * (uint32_t)n;

    memset(buf, 0, JK_FRAME_SIZE);
    memcpy(buf, JK_HEADER_RSP, 4);
    buf[4] = JK_FRAME_CELL_INFO;
    buf[5] = 1;
    for (i = 0; i < n; i++)
        put_u16(buf, 6 + (size_t)i * 2, mv);
    put_u32(buf, 70, (1U << n) - 1U);
    put_u16(buf, 74, mv);
    put_u16(buf, 76, 0);
    buf[78] = 0;
    buf[79] = 0;
    for (i = 0; i < n; i++)
        put_u16(buf, 80 + (size_t)i * 2, 18);
    put_i16(buf, 144, 280);
    put_u32(buf, 150, pack);
    put_i32(buf, 158, 12400);
    put_i16(buf, 162, 240);
    put_i16(buf, 164, 250);
    buf[173] = 87;
    put_u32(buf, 174, 174000);
    put_u32(buf, 178, 200000);
    buf[190] = 98;
    buf[198] = (uint8_t)c->charge;
    buf[199] = (uint8_t)c->discharge;
    buf[201] = (uint8_t)c->balance;
    buf[JK_CELL_CRC_OFFSET] = jk_crc8(buf, JK_CELL_CRC_OFFSET);
    c->frame_off = 0;
}

static void build_settings_frame(cli_t *c)
{
    uint8_t *buf = c->frame;

    memset(buf, 0, JK_FRAME_SIZE);
    memcpy(buf, JK_HEADER_RSP, 4);
    buf[4] = JK_FRAME_SETTINGS;
    buf[5] = 1;
    put_u32(buf, 10, 2500);
    put_u32(buf, 14, 2700);
    put_u32(buf, 18, c->ovp_mv ? c->ovp_mv : 3650);
    put_u32(buf, 22, c->ovpr_mv ? c->ovpr_mv : 3550);
    put_u32(buf, 26, c->trigger_mv ? c->trigger_mv : 10);
    put_u32(buf, 38, c->rcv_mv ? c->rcv_mv : 3600); /* RCV */
    put_u32(buf, 46, 2400);
    put_u32(buf, 50, 80000);
    put_u32(buf, 62, 80000);
    put_u32(buf, 78, 2000);
    put_u32(buf, 82, 600);
    put_u32(buf, 90, 600);
    put_i32(buf, 98, -100);
    put_i32(buf, 106, 900);
    buf[114] = 16;
    buf[118] = 1;
    buf[122] = 1;
    buf[126] = (uint8_t)c->balance;
    put_u32(buf, 130, 200000);
    put_u32(buf, 138, c->start_mv ? c->start_mv : 3000);
    buf[JK_CELL_CRC_OFFSET] = jk_crc8(buf, JK_CELL_CRC_OFFSET);
    c->frame_off = 0;
}

static int json_str(const char *js, const char *key, char *dst, size_t cap)
{
    char pat[64];
    const char *p, *q;
    size_t n;
    dst[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return -1;
    p++;
    q = strchr(p, '"');
    if (!q)
        return -1;
    n = (size_t)(q - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
    return 0;
}

static int mac_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    return strcasecmp(a, b) == 0;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
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

static void queue_line(cli_t *c, const char *s)
{
    size_t n = strlen(s);
    if (c->out_len + n + 1 >= sizeof(c->out))
        return;
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
    c->out[c->out_len++] = '\n';
}

static void queue_ok(cli_t *c, const char *cmd)
{
    char line[160];
    snprintf(line, sizeof(line),
             "{\"type\":\"ok\",\"cmd\":\"%s\",\"address\":\"%s\"}",
             cmd, c->mac);
    queue_line(c, line);
}

static void queue_err(cli_t *c, const char *msg)
{
    char line[192];
    snprintf(line, sizeof(line),
             "{\"type\":\"error\",\"address\":\"%s\",\"message\":\"%s\"}",
             c->mac[0] ? c->mac : "", msg);
    queue_line(c, line);
}

static int mac_taken(const char *mac, int self)
{
    int i;
    for (i = 0; i < MAX_CLI; i++) {
        if (i == self || g_cli[i].fd < 0 || !g_cli[i].bound)
            continue;
        if (mac_eq(g_cli[i].mac, mac))
            return 1;
    }
    return 0;
}

static void seed_mac(cli_t *c)
{
    c->charge = 1;
    c->discharge = 1;
    c->balance = 0;
    c->ovp_mv = 3650;
    c->ovpr_mv = 3550;
    c->rcv_mv = 3600;
    c->trigger_mv = 10;
    c->start_mv = 3000;
    if (mac_eq(c->mac, MAC_A))
        c->cell_mv = 3300;
    else if (mac_eq(c->mac, MAC_B))
        c->cell_mv = 3400;
    else
        c->cell_mv = 3200;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void apply_write(cli_t *c, const uint8_t *cmd, int n)
{
    uint8_t reg, on;
    if (n < 7)
        return;
    if (memcmp(cmd, JK_HEADER_CMD, 4) != 0)
        return;
    reg = cmd[4];
    on = cmd[6] ? 1 : 0;
    if (reg == JK_REG_CHARGE)
        c->charge = on;
    else if (reg == JK_REG_DISCHARGE)
        c->discharge = on;
    else if (reg == JK_REG_BALANCE)
        c->balance = on;
    else if (reg == JK_REG_BALANCE_TRIGGER && n >= 10)
        c->trigger_mv = le32(cmd + 6);
    else if (reg == JK_REG_START_BALANCE_JK02_32S && n >= 10)
        c->start_mv = le32(cmd + 6);
    else if (reg == JK_REG_CELL_OVP && n >= 10)
        c->ovp_mv = le32(cmd + 6);
    else if (reg == JK_REG_CELL_OVPR && n >= 10)
        c->ovpr_mv = le32(cmd + 6);
    else if (reg == JK_REG_CELL_RCV && n >= 10)
        c->rcv_mv = le32(cmd + 6);
}

static void handle_line(cli_t *c, int idx, const char *line)
{
    char cmd[32], addr[40], hex[128];
    cmd[0] = addr[0] = hex[0] = '\0';
    (void)json_str(line, "cmd", cmd, sizeof(cmd));
    (void)json_str(line, "address", addr, sizeof(addr));
    (void)json_str(line, "hex", hex, sizeof(hex));
    if (strcmp(cmd, "quit") == 0)
        exit(0);
    if (strcmp(cmd, "drop") == 0) {
        const char *mac = addr[0] ? addr : c->mac;
        int j;

        if (!mac[0]) {
            queue_err(c, "missing address");
            return;
        }
        for (j = 0; j < MAX_CLI; j++) {
            if (g_cli[j].fd < 0 || !g_cli[j].bound)
                continue;
            if (!mac_eq(g_cli[j].mac, mac))
                continue;
            queue_err(&g_cli[j], "disconnected");
            g_cli[j].acked = 0;
            g_cli[j].frame_off = JK_FRAME_SIZE;
        }
        queue_ok(c, "drop");
        return;
    }
    if (strcmp(cmd, "connect") == 0) {
        if (!addr[0]) {
            queue_err(c, "missing address");
            return;
        }
        if (mac_taken(addr, idx)) {
            queue_err(c, "mac in use");
            return;
        }
        snprintf(c->mac, sizeof(c->mac), "%.*s", (int)sizeof(c->mac) - 1, addr);
        c->bound = 1;
        seed_mac(c);
        queue_ok(c, "connect");
        c->acked = 1;
        build_settings_frame(c);
        c->tick_wait = 2; /* flush ACK before the first notify */
        return;
    }
    if (!c->acked) {
        queue_err(c, "handshake required");
        return;
    }
    if (strcmp(cmd, "write") == 0) {
        uint8_t raw[64];
        int n;
        if (!mac_eq(addr, c->mac)) {
            queue_err(c, "address mismatch");
            return;
        }
        n = hex_decode(hex, raw, sizeof(raw));
        apply_write(c, raw, n);
        queue_ok(c, "write");
        /* 0x96/0x97 polls must not clobber an in-flight settings notify. */
        if (n >= 5 && (raw[4] == JK_REG_CELL_OVP || raw[4] == JK_REG_CELL_OVPR ||
                       raw[4] == JK_REG_CELL_RCV))
            build_settings_frame(c);
        else if (n >= 5 && (raw[4] == JK_REG_CHARGE ||
                            raw[4] == JK_REG_DISCHARGE ||
                            raw[4] == JK_REG_BALANCE ||
                            raw[4] == JK_REG_BALANCE_TRIGGER ||
                            raw[4] == JK_REG_START_BALANCE_JK02_32S))
            build_frame(c);
        return;
    }
    if (strcmp(cmd, "disconnect") == 0) {
        if (addr[0] && !mac_eq(addr, c->mac)) {
            queue_err(c, "address mismatch");
            return;
        }
        queue_ok(c, "disconnect");
        c->acked = 0;
        c->bound = 0;
        c->mac[0] = '\0';
        return;
    }
    if (strcmp(cmd, "scan") == 0) {
        queue_line(c,
                   "{\"type\":\"scan\",\"results\":["
                   "{\"address\":\"" MAC_A "\",\"name\":\"JK_BD6A24S8P\",\"rssi\":-67},"
                   "{\"address\":\"" MAC_B "\",\"name\":\"JK_BD6A24S8P\",\"rssi\":-72}"
                   "]}");
        return;
    }
}

static void queue_notify_chunk(cli_t *c)
{
    char hex[CHUNK * 2 + 1];
    char line[160];
    size_t n = CHUNK;
    if (!c->acked)
        return;
    if (c->tick_wait) {
        c->tick_wait--;
        return;
    }
    if (c->frame_off >= JK_FRAME_SIZE) {
        build_frame(c);
        c->tick_wait = 4;
        return;
    }
    if (c->frame_off + n > JK_FRAME_SIZE)
        n = JK_FRAME_SIZE - c->frame_off;
    hex_encode(c->frame + c->frame_off, n, hex, sizeof(hex));
    c->frame_off += n;
    snprintf(line, sizeof(line),
             "{\"type\":\"notify\",\"address\":\"%s\",\"hex\":\"%s\"}",
             c->mac, hex);
    queue_line(c, line);
}

static void cli_close(cli_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void cli_flush(cli_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            cli_close(c);
            return;
        }
        c->out_off += (size_t)n;
    }
    c->out_len = c->out_off = 0;
}

static void cli_read(cli_t *c, int idx)
{
    for (;;) {
        ssize_t n;
        char *nl;
        if (c->in_len + 1 >= sizeof(c->in))
            c->in_len = 0;
        n = read(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            cli_close(c);
            return;
        }
        if (n == 0) {
            cli_close(c);
            return;
        }
        c->in_len += (size_t)n;
        c->in[c->in_len] = '\0';
        while ((nl = memchr(c->in, '\n', c->in_len)) != NULL) {
            size_t ln = (size_t)(nl - c->in);
            c->in[ln] = '\0';
            if (ln && c->in[ln - 1] == '\r')
                c->in[ln - 1] = '\0';
            handle_line(c, idx, c->in);
            memmove(c->in, nl + 1, c->in_len - ln - 1);
            c->in_len -= ln + 1;
            c->in[c->in_len] = '\0';
        }
    }
}

static int bind_abs(const char *adapter)
{
    int fd, n;
    struct sockaddr_un a;
    socklen_t alen;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    n = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "mf-gatt/%s", adapter);
    if (n < 0 || (size_t)n >= sizeof(a.sun_path) - 1) {
        close(fd);
        return -1;
    }
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    if (bind(fd, (struct sockaddr *)&a, alen) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int i, lfd;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--adapter") && i + 1 < argc)
            g_adapter = argv[++i];
        else if (!strncmp(argv[i], "--adapter=", 10))
            g_adapter = argv[i] + 10;
    }
    for (i = 0; i < MAX_CLI; i++)
        g_cli[i].fd = -1;
    lfd = bind_abs(g_adapter);
    if (lfd < 0) {
        fprintf(stderr, "fake_gatt: bind @mf-gatt/%s: %s\n",
                g_adapter, strerror(errno));
        return 1;
    }
    printf("fake_gatt: listening on @mf-gatt/%s\n", g_adapter);
    fflush(stdout);
    for (;;) {
        struct pollfd p[MAX_CLI + 1];
        int np = 1;
        p[0].fd = lfd;
        p[0].events = POLLIN;
        for (i = 0; i < MAX_CLI; i++) {
            if (g_cli[i].fd < 0)
                continue;
            p[np].fd = g_cli[i].fd;
            p[np].events = POLLIN;
            if (g_cli[i].out_len > g_cli[i].out_off)
                p[np].events = (short)(p[np].events | POLLOUT);
            np++;
        }
        if (poll(p, (nfds_t)np, 20) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (p[0].revents & POLLIN) {
            int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                int slot = -1;
                for (i = 0; i < MAX_CLI; i++) {
                    if (g_cli[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                    close(fd);
                else {
                    memset(&g_cli[slot], 0, sizeof(g_cli[slot]));
                    g_cli[slot].fd = fd;
                }
            }
        }
        for (i = 0; i < MAX_CLI; i++) {
            if (g_cli[i].fd < 0)
                continue;
            cli_read(&g_cli[i], i);
            if (g_cli[i].fd < 0)
                continue;
            if (g_cli[i].acked)
                queue_notify_chunk(&g_cli[i]);
            cli_flush(&g_cli[i]);
        }
    }
    close(lfd);
    return 0;
}
