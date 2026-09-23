/*
 * bms_proto.c -- Tianpower (TBD) BMS RS485 protocol implementation.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* cfmakeraw */

#include "bms_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOI 0x7E
#define EOI 0x0D

/* ------------------------------------------------------------------ */
/* Frame                                                              */
/* ------------------------------------------------------------------ */

uint8_t bms_check(const uint8_t *buf, size_t n)
{
    uint8_t  x = 0;
    unsigned s = 0;
    for (size_t i = 0; i < n; i++)
    {
        x ^= buf[i];
        s += buf[i];
    }
    return (uint8_t)((x ^ s) & 0xFF);
}

size_t bms_build_frame(uint8_t addr, uint8_t cmd,
                       const uint8_t *data, size_t dlen, uint8_t *out)
{
    out[0] = SOI;
    out[1] = addr;
    out[2] = cmd;
    out[3] = (uint8_t)dlen;
    if (dlen)
        memcpy(out + 4, data, dlen);
    size_t body = 4 + dlen;
    out[body]     = bms_check(out, body);
    out[body + 1] = EOI;
    return body + 2;
}

/* ------------------------------------------------------------------ */
/* Serial                                                             */
/* ------------------------------------------------------------------ */

static speed_t baud_to_speed(int baud)
{
    switch (baud)
    {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return 0;
    }
}

static int serial_open_internal(const char *path, int baud, bool nonblock)
{
    speed_t speed = baud_to_speed(baud);
    if (!speed)
    {
        fprintf(stderr, "unsupported baud rate: %d\n", baud);
        return -1;
    }
    int flags = O_RDWR | O_NOCTTY;
    if (nonblock) flags |= O_NONBLOCK;
    int fd = open(path, flags);
    if (fd < 0)
    {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
    {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
    tio.c_cflag &= ~(tcflag_t)PARENB;
    tio.c_cflag &= ~(tcflag_t)CSTOPB;
    tio.c_cflag &= ~(tcflag_t)CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int bms_serial_open(const char *path, int baud)
{
    return serial_open_internal(path, baud, false);
}

int bms_serial_open_nonblock(const char *path, int baud)
{
    return serial_open_internal(path, baud, true);
}

void bms_init(bms_t *b, int fd, uint8_t addr, bool debug)
{
    b->fd        = fd;
    b->addr      = addr;
    b->debug     = debug;
    b->timeout   = 1.5;
    b->tx_settle = 0.25;
}

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int serial_read_exact(int fd, uint8_t *out, size_t n, double timeout)
{
    size_t got = 0;
    double deadline = now_seconds() + timeout;
    while (got < n)
    {
        double remaining = deadline - now_seconds();
        if (remaining <= 0) return (int)got;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = (time_t)remaining;
        tv.tv_usec = (suseconds_t)((remaining - (double)tv.tv_sec) * 1e6);
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rv == 0) return (int)got;
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static void hex_dump(FILE *f, const char *tag, const uint8_t *buf, size_t n)
{
    fprintf(f, "%s ", tag);
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%02x%s", buf[i], i + 1 == n ? "" : " ");
    fputc('\n', f);
}

/* ------------------------------------------------------------------ */
/* TX/RX                                                              */
/* ------------------------------------------------------------------ */

int bms_txrx(bms_t *b, uint8_t cmd,
             const uint8_t *data, size_t dlen, uint8_t *body_out)
{
    if (dlen > 0xFF)
    {
        fprintf(stderr, "data too long: %zu\n", dlen);
        return -1;
    }
    uint8_t frame[BMS_MAX_FRAME];
    size_t  flen = bms_build_frame(b->addr, cmd, data, dlen, frame);
    if (b->debug) hex_dump(stderr, "TX ", frame, flen);

    tcflush(b->fd, TCIOFLUSH);
    if (write(b->fd, frame, flen) != (ssize_t)flen)
    {
        fprintf(stderr, "write: %s\n", strerror(errno));
        return -1;
    }
    struct timespec ts = { 0, (long)(b->tx_settle * 1e9) };
    nanosleep(&ts, NULL);

    uint8_t head[4];
    int rh = serial_read_exact(b->fd, head, 4, b->timeout + 0.5);
    if (rh != 4)
    {
        fprintf(stderr, "timeout: got %d/4 header bytes\n", rh);
        return -1;
    }
    if (head[0] != SOI)
    {
        fprintf(stderr, "bad SOI: 0x%02x\n", head[0]);
        return -1;
    }
    uint8_t length = head[3];
    if ((size_t)length + 6 > BMS_MAX_FRAME)
    {
        fprintf(stderr, "frame too large: LEN=%u\n", length);
        return -1;
    }

    uint8_t tail[BMS_MAX_PAYLOAD + 2];
    int rr = serial_read_exact(b->fd, tail, (size_t)length + 2, b->timeout + 0.5);
    if (rr != length + 2)
    {
        fprintf(stderr, "timeout: got %d/%d payload bytes\n", rr, length + 2);
        return -1;
    }

    if (b->debug)
    {
        uint8_t whole[BMS_MAX_FRAME];
        memcpy(whole, head, 4);
        memcpy(whole + 4, tail, (size_t)length + 2);
        hex_dump(stderr, "RX ", whole, (size_t)length + 6);
    }

    if (head[1] != b->addr)
    {
        fprintf(stderr, "reply addr %u != %u\n", head[1], b->addr);
        return -1;
    }
    if (head[2] != cmd)
    {
        fprintf(stderr, "reply cmd 0x%02x != 0x%02x\n", head[2], cmd);
        return -1;
    }
    if (tail[length + 1] != EOI)
    {
        fprintf(stderr, "bad EOI: 0x%02x\n", tail[length + 1]);
        return -1;
    }

    uint8_t whole[BMS_MAX_FRAME];
    memcpy(whole, head, 4);
    memcpy(whole + 4, tail, length);
    uint8_t expect = bms_check(whole, (size_t)length + 4);
    if (tail[length] != expect)
    {
        fprintf(stderr, "checksum mismatch: got 0x%02x, expected 0x%02x\n",
                tail[length], expect);
        return -1;
    }

    memcpy(body_out, tail, length);
    return length;
}

/* ------------------------------------------------------------------ */
/* Non-blocking txrx state machine                                    */
/* ------------------------------------------------------------------ */

void bms_txrx_init(bms_txrx_state_t *st, int fd, uint8_t addr, uint8_t cmd,
                   const uint8_t *data, size_t dlen,
                   double timeout, double settle)
{
    memset(st, 0, sizeof(*st));
    st->fd      = fd;
    st->addr    = addr;
    st->cmd     = cmd;
    st->timeout = timeout > 0 ? timeout : 1.5;
    st->deadline     = now_seconds() + st->timeout + settle + 0.5;
    st->settle_until = 0;
    st->phase = 0;
    if (dlen > 0xFF)
    {
        snprintf(st->err, sizeof(st->err), "data too long: %zu", dlen);
        st->phase = -1;
        return;
    }
    st->tx_len = bms_build_frame(addr, cmd, data, dlen, st->tx);
    tcflush(fd, TCIOFLUSH);
    (void)settle;  /* assigned via deadline above; set settle_until once tx done */
    st->settle_until = settle;  /* temporarily store settle interval here */
}

bms_io_t bms_txrx_step(bms_txrx_state_t *st)
{
    if (st->phase < 0) return BMS_IO_ERROR;

    double now = now_seconds();
    if (now > st->deadline)
    {
        snprintf(st->err, sizeof(st->err), "timeout in phase %d", st->phase);
        return BMS_IO_TIMEOUT;
    }

    /* Phase 0: write TX frame, possibly across multiple calls. */
    if (st->phase == 0)
    {
        while (st->tx_sent < st->tx_len)
        {
            ssize_t w = write(st->fd, st->tx + st->tx_sent, st->tx_len - st->tx_sent);
            if (w > 0)
            {
                st->tx_sent += (size_t)w;
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return BMS_IO_NEED_WRITE;
            snprintf(st->err, sizeof(st->err), "write: %s", strerror(errno));
            return BMS_IO_ERROR;
        }
        /* Schedule settle: settle_until currently holds the interval, set deadline now. */
        st->settle_until = now_seconds() + st->settle_until;
        st->phase = 1;
    }

    /* Phase 1: wait for settle to elapse, then move to read. */
    if (st->phase == 1)
    {
        if (now_seconds() < st->settle_until)
            return BMS_IO_NEED_TIME;
        st->phase = 2;
    }

    /* Phase 2: read 4-byte header. */
    if (st->phase == 2)
    {
        while (st->rx_head_got < 4)
        {
            ssize_t r = read(st->fd, st->rx_head + st->rx_head_got, 4 - st->rx_head_got);
            if (r > 0)
            {
                st->rx_head_got += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r == 0 || (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
                return BMS_IO_NEED_READ;
            snprintf(st->err, sizeof(st->err), "read head: %s", strerror(errno));
            return BMS_IO_ERROR;
        }
        if (st->rx_head[0] != SOI)
        {
            snprintf(st->err, sizeof(st->err), "bad SOI: 0x%02x", st->rx_head[0]);
            return BMS_IO_ERROR;
        }
        if (st->rx_head[1] != st->addr)
        {
            snprintf(st->err, sizeof(st->err), "addr %u != %u",
                     st->rx_head[1], st->addr);
            return BMS_IO_ERROR;
        }
        if (st->rx_head[2] != st->cmd)
        {
            snprintf(st->err, sizeof(st->err), "cmd 0x%02x != 0x%02x",
                     st->rx_head[2], st->cmd);
            return BMS_IO_ERROR;
        }
        uint8_t len = st->rx_head[3];
        if ((size_t)len + 6 > BMS_MAX_FRAME)
        {
            snprintf(st->err, sizeof(st->err), "frame too large: LEN=%u", len);
            return BMS_IO_ERROR;
        }
        st->rx_tail_need = (size_t)len + 2;
        st->phase = 3;
    }

    /* Phase 3: read payload + CHK + EOI. */
    if (st->phase == 3)
    {
        while (st->rx_tail_got < st->rx_tail_need)
        {
            ssize_t r = read(st->fd, st->rx_tail + st->rx_tail_got,
                             st->rx_tail_need - st->rx_tail_got);
            if (r > 0)
            {
                st->rx_tail_got += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r == 0 || (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
                return BMS_IO_NEED_READ;
            snprintf(st->err, sizeof(st->err), "read body: %s", strerror(errno));
            return BMS_IO_ERROR;
        }
        size_t  body_len = st->rx_tail_need - 2;
        uint8_t whole[BMS_MAX_FRAME];
        memcpy(whole, st->rx_head, 4);
        memcpy(whole + 4, st->rx_tail, body_len);
        uint8_t want = bms_check(whole, body_len + 4);
        if (st->rx_tail[body_len] != want)
        {
            snprintf(st->err, sizeof(st->err),
                     "checksum mismatch: got 0x%02x want 0x%02x",
                     st->rx_tail[body_len], want);
            return BMS_IO_ERROR;
        }
        if (st->rx_tail[body_len + 1] != EOI)
        {
            snprintf(st->err, sizeof(st->err),
                     "bad EOI: 0x%02x", st->rx_tail[body_len + 1]);
            return BMS_IO_ERROR;
        }
        memcpy(st->body, st->rx_tail, body_len);
        st->body_len = (int)body_len;
        st->phase    = 4;
        return BMS_IO_DONE;
    }

    if (st->phase == 4) return BMS_IO_DONE;
    snprintf(st->err, sizeof(st->err), "invalid phase %d", st->phase);
    return BMS_IO_ERROR;
}

int bms_read_string(bms_t *b, uint8_t cmd, char *out, size_t cap)
{
    uint8_t body[BMS_MAX_PAYLOAD];
    int n = bms_txrx(b, cmd, NULL, 0, body);
    if (n < 0) return -1;
    int len = 0;
    while (len < n && body[len] != 0) len++;
    while (len > 0 && (body[len - 1] == ' ' || body[len - 1] == '\t'
                    || body[len - 1] == '\r' || body[len - 1] == '\n'))
        len--;
    if ((size_t)len + 1 > cap) len = (int)cap - 1;
    memcpy(out, body, (size_t)len);
    out[len] = 0;
    return len;
}

/* ------------------------------------------------------------------ */
/* Flag table                                                         */
/* ------------------------------------------------------------------ */

const bms_flag_t BMS_FLAGS[] = {
    { 0, 0x08, "cell_vol_low_fault" },
    { 0, 0x10, "vol_line_break" },
    { 0, 0x20, "charge_mos_fault" },
    { 0, 0x40, "discharge_mos_fault" },
    { 0, 0x80, "vol_sensor_fault" },
    { 1, 0x01, "ntc_disconnection" },
    { 1, 0x02, "adc_mod_fault" },
    { 1, 0x04, "reverse_battery" },
    { 1, 0x08, "fan_on_failure" },
    { 1, 0x10, "battery_locked" },
    { 2, 0x01, "disc_ov_temp_protection" },
    { 2, 0x02, "disc_un_temp_protection" },
    { 2, 0x08, "startup_failed" },
    { 2, 0x10, "charge_mos_off" },
    { 2, 0x20, "discharge_mos_off" },
    { 2, 0x40, "host_536_com_timeout" },
    { 3, 0x01, "charging" },
    { 3, 0x02, "discharging" },
    { 3, 0x04, "short_circuit_protection" },
    { 3, 0x08, "ov_cur_protection" },
    { 3, 0x40, "chg_ov_temp_protection" },
    { 3, 0x80, "chg_un_temp_protection" },
    { 4, 0x01, "ambient_low_temp_protection" },
    { 4, 0x02, "ambient_high_temp_protection" },
    { 4, 0x80, "heater_pad_indicator" },
    { 5, 0x01, "manual_chg_mos_open" },
    { 5, 0x02, "manual_chg_mos_off" },
    { 5, 0x04, "manual_disc_mos_open" },
    { 5, 0x08, "manual_disc_mos_off" },
    { 5, 0x10, "heating_pad" },
    { 5, 0x20, "mosfet_ov_temp_protection" },
    { 5, 0x40, "mosfet_lo_temp_protection" },
    { 5, 0x80, "chg_open_temp_too_low" },
    { 7, 0x01, "soc_low_alarm2" },
    { 7, 0x02, "vibration_alarm" },
    { 7, 0x08, "fire_fighting_alarm" },
    { 8, 0x01, "amb_over_temp_alarm" },
    { 8, 0x02, "amb_low_temp_alarm" },
    { 8, 0x04, "mos_over_temp_alarm" },
    { 8, 0x08, "soc_low_alarm" },
    { 8, 0x10, "vol_dif_alarm" },
    { 8, 0x20, "bat_disc_over_temp_alarm" },
    { 8, 0x40, "bat_disc_low_temp_alarm" },
    { 9, 0x01, "cell_over_vol_alarm" },
    { 9, 0x02, "cell_low_vol_alarm" },
    { 9, 0x04, "pack_over_vol_alarm" },
    { 9, 0x08, "pack_low_vol_alarm" },
    { 9, 0x10, "chg_over_cur_alarm" },
    { 9, 0x20, "disc_over_curr_alarm" },
    { 9, 0x40, "bat_over_temp_alarm" },
    { 9, 0x80, "bat_low_temp_alarm" },
    { 10, 0x01, "cell_over_protection" },
    { 10, 0x02, "pack_over_protection" },
    { 10, 0x04, "cell_under_protection" },
    { 10, 0x08, "pack_under_protection" },
    { 11, 0x20, "full_charge_protection" },
};
const size_t BMS_FLAGS_N = sizeof(BMS_FLAGS) / sizeof(BMS_FLAGS[0]);

bool bms_flag_set(const bms_realtime_t *r, const char *name)
{
    for (size_t i = 0; i < BMS_FLAGS_N; i++)
    {
        if (strcmp(BMS_FLAGS[i].name, name) == 0)
            return (r->raw_status[BMS_FLAGS[i].byte_idx] & BMS_FLAGS[i].mask) != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Parser                                                             */
/* ------------------------------------------------------------------ */

int bms_parse_realtime(const uint8_t *b, size_t len, bms_realtime_t *r)
{
    if (len < 2)
    {
        fprintf(stderr, "payload too short: %zu\n", len);
        return -1;
    }
    uint8_t n = b[1];
    if (n == 0 || n > BMS_MAX_CELLS)
    {
        fprintf(stderr, "implausible cell count: %u\n", n);
        return -1;
    }
    size_t idx_tcnt = 15 + 2u * n;
    if (idx_tcnt >= len)
    {
        fprintf(stderr, "payload too short for temp count\n");
        return -1;
    }
    uint8_t t = b[idx_tcnt];
    if (t < 2 || t > 6) t = 5;
    if (t > BMS_MAX_TEMPS) t = BMS_MAX_TEMPS;

    r->cell_count = n;
    r->temp_count = t;

    for (size_t i = 0; i < n; i++)
    {
        uint8_t hi = b[2 * i + 2];
        uint8_t lo = b[2 * i + 3];
        r->cell_voltages_v[i] = (double)(((hi & 0x1F) << 8) | lo) / 1000.0;
        r->cell_balancing[i]  = (hi & 0x80) != 0;
    }

    uint16_t raw_cur = (uint16_t)((b[4 + 2u * n] << 8) | b[5 + 2u * n]);
    r->current_a = (30000.0 - (double)raw_cur) / 100.0;

    r->soc_pct          = (double)((b[8 + 2u * n] << 8) | b[9 + 2u * n]) / 100.0;
    r->full_capacity_ah = (double)((b[12 + 2u * n] << 8) | b[13 + 2u * n]) / 100.0;
    r->remaining_capacity_ah = r->full_capacity_ah * r->soc_pct / 100.0;

    for (size_t i = 0; i < t; i++)
        r->temperatures_c[i] = (double)b[17 + 2u * n + 2u * i] - 50.0;

    size_t base = 2u * n + 2u * t;
    if (39u + base >= len)
    {
        fprintf(stderr, "payload too short for pack-level fields\n");
        return -1;
    }
    r->cycles         = (uint16_t)((b[30 + base] << 8) | b[31 + base]);
    r->pack_voltage_v = (double)((b[34 + base] << 8) | b[35 + base]) / 100.0;
    r->soh_pct        = (double)((b[38 + base] << 8) | b[39 + base]) / 100.0;

    memcpy(r->raw_status, b + 18 + base, 10);
    r->raw_status[10] = (42u + base < len) ? b[42u + base] : 0;
    r->raw_status[11] = (43u + base < len) ? b[43u + base] : 0;
    return 0;
}

int bms_read_realtime(bms_t *b, bms_realtime_t *r)
{
    uint8_t body[BMS_MAX_PAYLOAD];
    int n = bms_txrx(b, 0x01, NULL, 0, body);
    if (n < 0) return -1;
    return bms_parse_realtime(body, (size_t)n, r);
}
