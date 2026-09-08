/*
 * bms_proto.h -- Tianpower (TBD) BMS RS485 protocol.
 *
 * Ported from xd-battery. Used by libmf_battery_xd.so.
 */
#ifndef BMS_PROTO_H
#define BMS_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BMS_MAX_CELLS    32
#define BMS_MAX_TEMPS     8
#define BMS_MAX_PAYLOAD 256
#define BMS_MAX_FRAME   512

typedef struct {
    int     fd;
    uint8_t addr;
    bool    debug;
    double  timeout;       /* read timeout, seconds */
    double  tx_settle;     /* delay after TX before reading */
} bms_t;

typedef struct {
    uint8_t  cell_count;
    uint8_t  temp_count;
    double   cell_voltages_v[BMS_MAX_CELLS];
    bool     cell_balancing[BMS_MAX_CELLS];
    double   temperatures_c[BMS_MAX_TEMPS];
    double   current_a;
    double   soc_pct;
    double   soh_pct;
    double   full_capacity_ah;
    double   remaining_capacity_ah;
    double   pack_voltage_v;
    uint16_t cycles;
    uint8_t  raw_status[12]; /* flogbuff[0..11] */
} bms_realtime_t;

typedef struct {
    uint8_t     byte_idx;
    uint8_t     mask;
    const char *name;
} bms_flag_t;

extern const bms_flag_t BMS_FLAGS[];
extern const size_t     BMS_FLAGS_N;

/* Frame primitives. */
uint8_t bms_check(const uint8_t *buf, size_t n);
size_t  bms_build_frame(uint8_t addr, uint8_t cmd,
                        const uint8_t *data, size_t dlen, uint8_t *out);

/* Serial / transport. Returns fd >= 0 on success, -1 with stderr message. */
int  bms_serial_open(const char *path, int baud);            /* blocking */
int  bms_serial_open_nonblock(const char *path, int baud);   /* O_NONBLOCK set */
void bms_init(bms_t *b, int fd, uint8_t addr, bool debug);

/* Send a frame, validate the reply, copy payload to body_out (>= BMS_MAX_PAYLOAD).
 * Returns payload length (0..255) on success, -1 on error. */
int bms_txrx(bms_t *b, uint8_t cmd, const uint8_t *data, size_t dlen,
             uint8_t *body_out);

/* High-level reads. */
int bms_read_string(bms_t *b, uint8_t cmd, char *out, size_t cap);
int bms_read_realtime(bms_t *b, bms_realtime_t *r);

/* ---------------- Non-blocking txrx state machine ---------------- */
/*
 * Drive one request/response on a non-blocking fd. Caller uses select()
 * (or equivalent) to know when the fd is readable/writable and then calls
 * bms_txrx_step() until it returns DONE or an error.
 *
 * Lifecycle:
 *     bms_txrx_init(&st, fd, addr, cmd, data, dlen, timeout_s, settle_s);
 *     while ((s = bms_txrx_step(&st)) == BMS_IO_NEED_READ ||
 *            s == BMS_IO_NEED_WRITE ||
 *            s == BMS_IO_NEED_TIME) {
 *         // sleep / select on st.fd as appropriate, then loop
 *     }
 *     if (s == BMS_IO_DONE) {
 *         // st.body / st.body_len are valid
 *     } else {
 *         // st.err describes the failure
 *     }
 */
typedef enum {
    BMS_IO_DONE       = 0,    /* response complete and validated */
    BMS_IO_NEED_READ  = 1,    /* fd would block on read, retry later */
    BMS_IO_NEED_WRITE = 2,    /* fd would block on write, retry later */
    BMS_IO_NEED_TIME  = 3,    /* TX settle delay not yet elapsed */
    BMS_IO_TIMEOUT    = 4,    /* gave up waiting */
    BMS_IO_ERROR      = 5,    /* protocol or system error */
} bms_io_t;

typedef struct {
    int      fd;
    uint8_t  addr;
    uint8_t  cmd;
    double   timeout;
    double   deadline;        /* monotonic seconds */
    double   settle_until;    /* monotonic seconds */

    uint8_t  tx[BMS_MAX_FRAME];
    size_t   tx_len;
    size_t   tx_sent;

    uint8_t  rx_head[4];
    size_t   rx_head_got;

    uint8_t  rx_tail[BMS_MAX_PAYLOAD + 2];
    size_t   rx_tail_need;     /* length + 2 (payload + CHK + EOI) */
    size_t   rx_tail_got;

    int      phase;            /* 0 write, 1 settle, 2 head, 3 tail, 4 done */

    uint8_t  body[BMS_MAX_PAYLOAD];
    int      body_len;
    char     err[96];
} bms_txrx_state_t;

void     bms_txrx_init(bms_txrx_state_t *st, int fd, uint8_t addr, uint8_t cmd,
                       const uint8_t *data, size_t dlen,
                       double timeout, double settle);
bms_io_t bms_txrx_step(bms_txrx_state_t *st);

/* Parse a cmd 0x01 payload into a realtime struct. Returns 0 on success. */
int bms_parse_realtime(const uint8_t *body, size_t len, bms_realtime_t *r);

/* Lookup a status-flag bit by name. */
bool bms_flag_set(const bms_realtime_t *r, const char *name);

#endif /* BMS_PROTO_H */
