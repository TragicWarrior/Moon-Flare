#ifndef JK_PROTO_H
#define JK_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Frame geometry */
#define JK_FRAME_SIZE          300
#define JK_CMD_FRAME_SIZE      20
#define JK_CELL_CRC_OFFSET     299
#define JK_CMD_CRC_OFFSET      19

/* Headers */
extern const uint8_t JK_HEADER_RSP[4];
extern const uint8_t JK_HEADER_CMD[4];
#define JK_AT_JUNK_SIZE 4

/* Frame type bytes (data[4]) */
#define JK_FRAME_CELL_INFO     0x02
#define JK_FRAME_DEVICE_INFO   0x03
#define JK_FRAME_SETTINGS      0x01
#define JK_FRAME_LOGBOOK       0x05

/* Command bytes for queries */
#define JK_CMD_CELL_INFO       0x96
#define JK_CMD_DEVICE_INFO     0x97
#define JK_CMD_LOGBOOK         0xA1

/* Register addresses for writes */
#define JK_REG_BALANCE_TRIGGER 0x06
#define JK_REG_START_BALANCE_JK02_32S  0x22
#define JK_REG_START_BALANCE_JK02_24S  0x26

/* Python models.clamp_trigger_v / clamp_start_v (saturate, 3 decimals). */
#define JK_TRIGGER_MIN_V 0.003
#define JK_TRIGGER_MAX_V 1.0
#define JK_START_MIN_V   1.20
#define JK_START_MAX_V   4.25

/* MOSFET switch registers */
#define JK_REG_CHARGE          0x1D
#define JK_REG_DISCHARGE       0x1E
#define JK_REG_BALANCE         0x1F

/* Protocol versions */
typedef enum {
    JK_PROTO_JK02_32S,
    JK_PROTO_JK02_24S
} jk_proto_t;

/* Balancing action values */
typedef enum {
    JK_BALANCE_OFF      = 0,
    JK_BALANCE_CHARGING = 1,
    JK_BALANCE_DISCHG   = 2
} jk_balancing_action_t;

/* Temperature sensor absent sentinel */
#define JK_TEMP_ABSENT  -2000

/* ---- Cell ---- */
typedef struct {
    uint8_t  index;             /* 1-based */
    float    voltage_v;
    float    resistance_ohm;
    bool     enabled;
    bool     balancing;
} jk_cell_t;

/* ---- CellInfo (decoded 0x02 frame) ---- */
typedef struct {
    uint8_t  frame_counter;
    jk_cell_t cells[32];
    uint8_t  cell_count;
    uint32_t enabled_mask;
    float    average_cell_v;
    float    delta_cell_v;
    uint8_t  max_cell_number;
    uint8_t  min_cell_number;
    float    mosfet_temp_c;
    uint32_t wire_resistance_warnings;
    float    pack_voltage_v;
    float    pack_power_w;
    float    current_a;
    float    temp1_c;
    float    temp2_c;
    float    temp3_c;
    float    temp4_c;
    float    temp5_c;
    uint32_t errors_bitmask;
    float    balance_current_a;
    jk_balancing_action_t balancing_action;
    float    soc_pct;
    float    remaining_ah;
    float    nominal_ah;
    uint32_t cycle_count;
    float    cycle_capacity_ah;
    float    soh_pct;
    bool     precharge_on;
    uint32_t runtime_s;
    bool     charge_mosfet_on;
    bool     discharge_mosfet_on;
    bool     precharging;
    bool     balancing_indicator;
    bool     heating_on;
    uint16_t emergency_countdown_s;
    uint8_t  charge_status;
    const uint8_t *raw;
} jk_cell_info_t;

/* ---- DeviceInfo (decoded 0x03 frame) ---- */
typedef struct {
    char vendor[17];
    char hardware_version[9];
    char software_version[9];
    uint32_t uptime_s;
    uint32_t power_on_count;
    char device_name[17];
    char device_passcode[17];
    char manufacturing_date[9];
    char serial_number[17];
    char user_data[17];
    const uint8_t *raw;
} jk_device_info_t;

/* ---- Settings (decoded 0x01 frame) ---- */
typedef struct {
    float    cell_uvp_v;
    float    cell_uvpr_v;
    float    cell_ovp_v;
    float    cell_ovpr_v;
    float    balance_trigger_v;
    float    start_balance_v;
    float    power_off_v;
    float    max_charge_a;
    float    max_discharge_a;
    float    max_balance_a;
    float    charge_otp_c;
    float    discharge_otp_c;
    float    charge_utp_c;
    float    power_tube_otp_c;
    uint8_t  cell_count;
    bool     charge_switch;
    bool     discharge_switch;
    bool     balancer_switch;
    float    nominal_ah;
    const uint8_t *raw;
} jk_settings_t;

/* ---- Assembler state ---- */
typedef struct {
    uint8_t buf[512];
    size_t  buf_len;
    uint32_t dropped;
    uint32_t corrupt;
} jk_frame_assembler_t;

/* ---- Error codes ---- */
typedef enum {
    JK_OK          =  0,
    JK_EBADLEN     = -1,
    JK_EBADHDR     = -2,
    JK_EBADTYPE    = -3,
    JK_ECRC        = -4,
    JK_EIMPLAUSIBLE= -5,
    JK_ENOMEM      = -6
} jk_result_t;

/* ---- Public API ---- */

/* CRC-8 truncated sum: sum of every byte, masked to 8 bits. */
uint8_t jk_crc8(const uint8_t *data, size_t len);

/* Build a 20-byte command frame.
 * cmd: command byte (0-255)
 * value: optional payload (up to 13 bytes), or NULL/0 for empty
 * counter: placed at offset 16 when payload is empty
 * Returns pointer to 20-byte frame. Caller owns the buffer. */
const uint8_t *jk_build_command(uint8_t cmd,
                                const uint8_t *value, size_t vlen,
                                uint8_t counter);

/* Switch command: register 0x1D/0x1E/0x1F with 4-byte 0x01/0x00 payload.
 * reg: register address (JK_REG_CHARGE, JK_REG_DISCHARGE, JK_REG_BALANCE)
 * on: true = 1, false = 0 */
const uint8_t *jk_build_switch_cmd(uint8_t reg, bool on);

/* Register write: 4-byte LE uint32 (millivolts/milliamps).
 * reg: register address (JK_REG_BALANCE_TRIGGER=0x06, etc.)
 * value: value in millivolts/milliamps */
const uint8_t *jk_build_register_cmd(uint8_t reg, uint32_t value);

/* Saturate like Python clamp_trigger_v / clamp_start_v (round 3 decimals). */
double   jk_clamp_trigger_v(double volts);
double   jk_clamp_start_v(double volts);
uint32_t jk_volts_to_mv(double volts); /* nearest millivolt; no clamp */

/* --- Frame Assembler --- */

/* Initialise assembler state (call once). */
void jk_assembler_init(jk_frame_assembler_t *asm_);

/* Feed a GATT notification chunk.  Strips leading/trailing AT\r\n.
 * If a complete valid frame is found, copies it into frame_buf (caller-owned,
 * must be >= JK_FRAME_SIZE bytes).  Returns 1 if a frame was assembled, 0 otherwise. */
int jk_assembler_feed(jk_frame_assembler_t *asm_,
                      const uint8_t *chunk, size_t chunk_len,
                      uint8_t *frame_buf, int max_frames);

/* --- Decoders --- */

/* Decode cell-info frame.  data must be >= len bytes with
 * valid CRC at byte JK_CELL_CRC_OFFSET.  Returns JK_OK on success.
 * expected_cells: if non-zero, reject if decoded count differs. */
jk_result_t jk_decode_cell_info(const uint8_t *data, size_t len,
                                jk_proto_t proto,
                                uint8_t expected_cells,
                                jk_cell_info_t *out);

/* Decode device-info frame.  data must be >= len bytes. */
jk_result_t jk_decode_device_info(const uint8_t *data, size_t len,
                                  jk_device_info_t *out);

/* Decode settings frame.  data must be >= len bytes. */
jk_result_t jk_decode_settings(const uint8_t *data, size_t len,
                               jk_settings_t *out);

/* --- Helpers --- */

/* Read int16 LE from data at offset. */
static inline int16_t jk_i16(const uint8_t *data, size_t off)
{
    return (int16_t)((uint16_t)data[off] | ((uint16_t)data[off+1] << 8));
}

/* Read uint16 LE from data at offset. */
static inline uint16_t jk_u16(const uint8_t *data, size_t off)
{
    return (uint16_t)data[off] | ((uint16_t)data[off+1] << 8);
}

/* Read int32 LE from data at offset. */
static inline int32_t jk_i32(const uint8_t *data, size_t off)
{
    return (int32_t)( (uint32_t)data[off]
                    | ((uint32_t)data[off+1] << 8)
                    | ((uint32_t)data[off+2] << 16)
                    | ((uint32_t)data[off+3] << 24) );
}

/* Read uint32 LE from data at offset. */
static inline uint32_t jk_u32(const uint8_t *data, size_t off)
{
    return (uint32_t)data[off]
         | ((uint32_t)data[off+1] << 8)
         | ((uint32_t)data[off+2] << 16)
         | ((uint32_t)data[off+3] << 24);
}

/* Extract null-terminated ASCII string from data[offset..offset+len-1].
 * Writes at most max_len-1 chars plus NUL. */
void jk_cstring(const uint8_t *data, size_t offset, size_t len,
                char *out, size_t max_len);

/* Decode temperature: int16 LE at offset, scaled by 0.1, or NaN if absent. */
float jk_temp_c(const uint8_t *data, size_t off);

#endif /* JK_PROTO_H */
