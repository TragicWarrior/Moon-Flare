#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jk_proto.h"

/* ---- Helpers (mirrors test_protocol.py) ---- */

static void put_u16(uint8_t *buf, size_t off, uint16_t val)
{
    buf[off] = (uint8_t)val;
    buf[off+1] = (uint8_t)(val >> 8);
}

static void put_u32(uint8_t *buf, size_t off, uint32_t val)
{
    buf[off]   = (uint8_t)val;
    buf[off+1] = (uint8_t)(val >> 8);
    buf[off+2] = (uint8_t)(val >> 16);
    buf[off+3] = (uint8_t)(val >> 24);
}

static void put_i16(uint8_t *buf, size_t off, int16_t val)
{
    uint16_t uv = (uint16_t)val;
    put_u16(buf, off, uv);
}

static void put_i32(uint8_t *buf, size_t off, int32_t val)
{
    uint32_t uv = (uint32_t)val;
    put_u32(buf, off, uv);
}

static void put_str(uint8_t *buf, size_t off, size_t len, const char *text)
{
    size_t n = 0;
    while (text[n] && n < len - 1) { buf[off+n] = (uint8_t)text[n]; n++; }
    buf[off+n] = '\0';
}

static void finish_crc(uint8_t *buf)
{
    buf[JK_CELL_CRC_OFFSET] = jk_crc8(buf, JK_CELL_CRC_OFFSET);
}

/* Build a cell-info frame with given parameters.
 * Mirrors test_protocol.py::make_cell_info_frame() */
static uint8_t *_make_cell_info_frame(
    const uint16_t *voltages_mv, uint8_t n_cells,
    int32_t current_ma, uint8_t soc, uint32_t cycles,
    uint32_t errors, uint8_t charge_mos, uint8_t discharge_mos,
    uint8_t bal_action, int16_t bal_ma)
{
    uint8_t *buf = calloc(1, JK_FRAME_SIZE);

    memcpy(buf, JK_HEADER_RSP, 4);
    buf[4] = JK_FRAME_CELL_INFO;
    buf[5] = 1;  /* frame counter */

    /* Cell voltages at offset 6+i*2 */
    for (uint8_t i = 0; i < n_cells; i++)
        put_u16(buf, 6 + i * 2, voltages_mv ? voltages_mv[i] : 3300);

    /* Enabled mask */
    uint32_t mask = (n_cells >= 32) ? 0xFFFFFFFFU : ((1U << n_cells) - 1);
    put_u32(buf, 70, mask);

    /* Average / delta / min-max cell number */
    uint16_t sum = 0;
    uint16_t min_v = 0xFFFF, max_v = 0;
    for (uint8_t i = 0; i < n_cells; i++) {
        uint16_t mv = voltages_mv ? voltages_mv[i] : 3300;
        sum += mv;
        if (mv < min_v) min_v = mv;
        if (mv > max_v) max_v = mv;
    }
    put_u16(buf, 74, sum / n_cells);
    put_u16(buf, 76, max_v - min_v);
    buf[78] = voltages_mv ?
              (uint8_t)(max_v - voltages_mv[0]) : 0;  /* approximate index */
    buf[79] = voltages_mv ?
              (uint8_t)(min_v - voltages_mv[0]) : 0;
    if (!voltages_mv) { buf[78] = 0; buf[79] = 0; }
    else {
        for (uint8_t i = 0; i < n_cells; i++) {
            if (voltages_mv[i] == max_v) buf[78] = i;
            if (voltages_mv[i] == min_v) buf[79] = i;
        }
    }

    /* Cell resistances at 80+i*2 */
    for (uint8_t i = 0; i < n_cells; i++)
        put_u16(buf, 80 + i * 2, 18);  /* 0.018 ohm */

    /* MOSFET temp: 28.0 C -> i16(280) */
    put_i16(buf, 144, 280);

    /* Pack voltage: sum of cell mV */
    uint32_t pack_mv = 0;
    for (uint8_t i = 0; i < n_cells; i++)
        pack_mv += voltages_mv ? voltages_mv[i] : 3300;
    put_u32(buf, 150, pack_mv);

    /* Current */
    put_i32(buf, 158, current_ma);

    /* Temperatures */
    put_i16(buf, 162, 240);  /* T1 24.0 C */
    put_i16(buf, 164, 250);  /* T2 25.0 C */

    /* Errors bitmask */
    put_u32(buf, 166, errors);

    /* Balance current */
    put_i16(buf, 170, bal_ma);

    /* Balancing action, SOC */
    buf[172] = bal_action;
    buf[173] = soc;

    /* Remaining / nominal mAh */
    put_u32(buf, 174, 174000);
    put_u32(buf, 178, 200000);

    /* Cycle count */
    put_u32(buf, 182, cycles);

    /* Cycle capacity */
    put_u32(buf, 186, 8400000);

    /* SOH */
    buf[190] = 98;

    /* Runtime */
    put_u32(buf, 194, 3600);

    /* MOSFETs */
    buf[198] = charge_mos;
    buf[199] = discharge_mos;

    /* Balancing indicator */
    buf[201] = bal_action ? 1 : 0;

    finish_crc(buf);
    return buf;
}

static uint8_t *_make_cell_info_default(void)
{
    return _make_cell_info_frame(NULL, 16, 12400, 87, 42,
                                  0, 1, 1, 1, 800);
}

static uint8_t *_make_device_info_frame(void)
{
    uint8_t *buf = calloc(1, JK_FRAME_SIZE);

    memcpy(buf, JK_HEADER_RSP, 4);
    buf[4] = JK_FRAME_DEVICE_INFO;
    buf[5] = 1;

    put_str(buf, 6, 16, "JK-BD6A24S8P");
    put_str(buf, 22, 8, "11.XW");
    put_str(buf, 30, 8, "11.48");
    put_u32(buf, 38, 12345);
    put_u32(buf, 42, 7);
    put_str(buf, 46, 16, "JK_BD6A24S8P");
    put_str(buf, 62, 16, "1234");
    put_str(buf, 78, 8, "240315");
    put_str(buf, 86, 16, "SN001");

    finish_crc(buf);
    return buf;
}

static uint8_t *_make_settings_frame(void)
{
    uint8_t *buf = calloc(1, JK_FRAME_SIZE);

    memcpy(buf, JK_HEADER_RSP, 4);
    buf[4] = JK_FRAME_SETTINGS;
    buf[5] = 1;

    put_u32(buf, 10, 2500);   /* UVP */
    put_u32(buf, 14, 2700);   /* UVPR */
    put_u32(buf, 18, 3650);   /* OVP */
    put_u32(buf, 22, 3550);   /* OVPR */
    put_u32(buf, 26, 10);     /* balance trigger */
    put_u32(buf, 46, 2400);   /* power off */
    put_u32(buf, 50, 80000);  /* max charge mA */
    put_u32(buf, 62, 80000);  /* max discharge mA */
    put_u32(buf, 78, 2000);   /* max balance mA */
    put_u32(buf, 82, 600);    /* charge OTP */
    put_u32(buf, 90, 600);    /* discharge OTP */
    put_i32(buf, 98, -100);   /* charge UTP */
    put_i32(buf, 106, 900);   /* power tube OTP */
    buf[114] = 16;             /* cell count */
    buf[118] = 1;              /* charge switch */
    buf[122] = 1;              /* discharge switch */
    buf[126] = 1;              /* balancer switch */
    put_u32(buf, 130, 200000); /* nominal mAh */
    put_u32(buf, 138, 3000);   /* start balance mV */

    finish_crc(buf);
    return buf;
}

/* ---- Test macros ---- */

#define TOL_F 0.01f  /* 10 mV tolerance for voltage comparisons */

static int _tests_run = 0;
static int _tests_fail = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(fn) do { \
    _tests_run++; \
    fn(); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        _tests_fail++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_OK(rc) ASSERT((rc) == JK_OK, "expected JK_OK")
#define ASSERT_ERR(rc) ASSERT((rc) != JK_OK, "expected error")
#define ASSERT_FLOAT_EQ(a, b, msg) ASSERT(((a) > (b) - TOL_F) && ((a) < (b) + TOL_F), msg)

/* ---- Tests ---- */

/* Test: build_command produces 20-byte frame with correct header, cmd, CRC. */
TEST(test_build_command_length_and_crc)
{
    const uint8_t *frame = jk_build_command(JK_CMD_CELL_INFO, NULL, 0, 0);
    ASSERT_EQ(strlen((const char*)frame + 4), strlen((const char*)frame + 4),
              "frame pointer non-NULL");
    ASSERT(frame != NULL, "frame non-NULL");
    ASSERT(frame[0] == 0xAA && frame[1] == 0x55 &&
           frame[2] == 0x90 && frame[3] == 0xEB, "header");
    ASSERT(frame[4] == JK_CMD_CELL_INFO, "cmd byte");
    ASSERT(frame[5] == 0, "payload len zero");
    ASSERT(frame[19] == jk_crc8(frame, JK_CMD_CRC_OFFSET), "CRC");
}

/* Test: counter placed at offset 16 for empty-payload commands. */
TEST(test_build_command_counter)
{
    const uint8_t *frame = jk_build_command(JK_CMD_DEVICE_INFO, NULL, 0, 3);
    ASSERT(frame[4] == JK_CMD_DEVICE_INFO, "cmd byte");
    ASSERT(frame[16] == 3, "counter at offset 16");
    ASSERT(frame[19] == jk_crc8(frame, JK_CMD_CRC_OFFSET), "CRC");
}

/* Test: switch commands for charge/discharge/balance. */
TEST(test_switch_commands)
{
    const uint8_t *on = jk_build_switch_cmd(JK_REG_CHARGE, true);
    ASSERT(on[4] == JK_REG_CHARGE, "charge reg");
    ASSERT(on[5] == 4, "charge payload len");
    ASSERT(on[6] == 1, "charge on");
    ASSERT(on[19] == jk_crc8(on, JK_CMD_CRC_OFFSET), "charge CRC");

    const uint8_t *off = jk_build_switch_cmd(JK_REG_DISCHARGE, false);
    ASSERT(off[4] == JK_REG_DISCHARGE, "discharge reg");
    ASSERT(off[6] == 0, "discharge off");
    ASSERT(off[19] == jk_crc8(off, JK_CMD_CRC_OFFSET), "discharge CRC");

    const uint8_t *bal_on = jk_build_switch_cmd(JK_REG_BALANCE, true);
    ASSERT(bal_on[4] == JK_REG_BALANCE, "balance reg");
    ASSERT(bal_on[6] == 1, "balance on");
}

/* Test: register writes for balance trigger (0x06) and start balance (0x22). */
TEST(test_register_writes)
{
    const uint8_t *trig = jk_build_register_cmd(JK_REG_BALANCE_TRIGGER, 30);
    ASSERT(trig[4] == JK_REG_BALANCE_TRIGGER, "reg 0x06");
    ASSERT(trig[5] == 4, "payload len");
    ASSERT(trig[6] == 30 && trig[7] == 0 && trig[8] == 0 && trig[9] == 0,
           "LE 30");
    ASSERT(trig[19] == jk_crc8(trig, JK_CMD_CRC_OFFSET), "CRC");

    const uint8_t *start = jk_build_register_cmd(JK_REG_START_BALANCE_JK02_32S, 3000);
    ASSERT(start[4] == JK_REG_START_BALANCE_JK02_32S, "reg 0x22");
    uint32_t val = start[6] | (start[7]<<8) | (start[8]<<16) | (start[9]<<24);
    ASSERT_EQ(val, 3000U, "LE 3000");
}

/* Test: decode_cell_info with 16 equal-voltage cells. */
TEST(test_decode_cell_info_16s)
{
    uint8_t *raw = _make_cell_info_default();
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_OK(rc);
    ASSERT(info.cell_count == 16, "16 cells");
    ASSERT_FLOAT_EQ(info.cells[0].voltage_v, 3.300f, "cell[0] 3.300V");
    ASSERT_FLOAT_EQ(info.pack_voltage_v, 52.800f, "pack 52.8V");
    ASSERT_FLOAT_EQ(info.current_a, 12.400f, "current 12.4A");
    ASSERT(info.soc_pct == 87, "SOC 87");
    ASSERT(info.soh_pct == 98, "SOH 98");
    ASSERT(info.cycle_count == 42, "cycles 42");
    ASSERT(info.charge_mosfet_on == true, "charge MOS on");
    ASSERT(info.discharge_mosfet_on == true, "discharge MOS on");
    ASSERT(info.balancing_action == JK_BALANCE_CHARGING, "balance charging");
    ASSERT_FLOAT_EQ(info.balance_current_a, 0.800f, "bal current 0.8A");
    ASSERT_FLOAT_EQ(info.mosfet_temp_c, 28.0f, "MOS temp 28C");
    ASSERT_FLOAT_EQ(info.temp1_c, 24.0f, "T1 24C");
    ASSERT_FLOAT_EQ(info.temp2_c, 25.0f, "T2 25C");
    ASSERT_FLOAT_EQ(info.nominal_ah, 200.0f, "nominal 200Ah");

    /* Balancing heuristic: all cells equal, balancer active -> all marked */
    ASSERT(info.cells[0].balancing == true, "balancing heuristic");

    free(raw);
}

/* Test: cell info with spread — min and max at expected positions. */
TEST(test_decode_cell_info_spread)
{
    uint16_t voltages[16];
    for (int i = 0; i < 16; i++) voltages[i] = 3300;
    voltages[2] = 3370;  /* max at index 3 (1-based) */
    voltages[10] = 3250; /* min at index 11 (1-based) */

    uint8_t *raw = _make_cell_info_frame(voltages, 16, 12400, 87, 42,
                                          0, 1, 1, 0, 0);
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_OK(rc);

    /* Find actual min/max from decoded cells */
    int max_idx = 0, min_idx = 0;
    float max_v = 0, min_v = 10.0f;
    for (uint8_t i = 0; i < info.cell_count; i++) {
        if (info.cells[i].voltage_v > max_v) { max_v = info.cells[i].voltage_v; max_idx = i; }
        if (info.cells[i].voltage_v < min_v) { min_v = info.cells[i].voltage_v; min_idx = i; }
    }
    ASSERT(info.cells[max_idx].index == 3, "max cell index 3");
    ASSERT(info.cells[min_idx].index == 11, "min cell index 11");

    float spread = (max_v - min_v) * 1000.0f;
    ASSERT_FLOAT_EQ(spread, 120.0f, "spread 120mV");

    /* No balancing when balancer is off */
    for (uint8_t i = 0; i < info.cell_count; i++)
        ASSERT(info.cells[i].balancing == false, "not balancing");

    free(raw);
}

/* Test: CRC mismatch is rejected. */
TEST(test_crc_mismatch)
{
    uint8_t *raw = _make_cell_info_default();
    raw[299] ^= 0xFF;  /* corrupt CRC */
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_ERR(rc);
    ASSERT(rc == JK_ECRC, "error is bad CRC");
    free(raw);
}

/* Test: implausible pack voltage vs cell sum is rejected. */
TEST(test_implausible_pack_vs_cells)
{
    uint8_t *raw = _make_cell_info_default();
    put_u32(raw, 150, 1000000);  /* 1000 V — impossible */
    finish_crc(raw);
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_ERR(rc);
    ASSERT(rc == JK_EIMPLAUSIBLE, "implausible pack voltage rejected");
    free(raw);
}

/* Test: error bits from cell-info frame. */
TEST(test_error_bits)
{
    uint8_t *raw = _make_cell_info_frame(NULL, 16, 12400, 87, 42,
                                          (1U << 11) | (1U << 5),
                                          1, 1, 1, 800);
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_OK(rc);

    /* Bit 11 = cell undervoltage, bit 5 = pack overvoltage */
    uint32_t bm = info.errors_bitmask;
    ASSERT((bm & (1U << 11)) != 0, "bit 11 cell UV");
    ASSERT((bm & (1U << 5)) != 0, "bit 5 pack OV");
    free(raw);
}

/* Test: decode_device_info. */
TEST(test_decode_device_info)
{
    uint8_t *raw = _make_device_info_frame();
    jk_device_info_t info;
    jk_result_t rc = jk_decode_device_info(raw, JK_FRAME_SIZE, &info);
    ASSERT_OK(rc);

    ASSERT(strcmp(info.vendor, "JK-BD6A24S8P") == 0, "vendor");
    ASSERT(strcmp(info.hardware_version, "11.XW") == 0, "hw version");
    ASSERT(strcmp(info.software_version, "11.48") == 0, "sw version");
    ASSERT(info.uptime_s == 12345, "uptime");
    ASSERT(info.power_on_count == 7, "power on count");
    ASSERT(strcmp(info.device_passcode, "1234") == 0, "passcode");
    ASSERT(strcmp(info.serial_number, "SN001") == 0, "serial");

    free(raw);
}

/* Test: decode_settings. */
TEST(test_decode_settings)
{
    uint8_t *raw = _make_settings_frame();
    jk_settings_t s;
    jk_result_t rc = jk_decode_settings(raw, JK_FRAME_SIZE, &s);
    ASSERT_OK(rc);

    ASSERT(s.cell_count == 16, "cell count 16");
    ASSERT_FLOAT_EQ(s.cell_uvp_v, 2.500f, "UVP 2.5V");
    ASSERT_FLOAT_EQ(s.cell_ovp_v, 3.650f, "OVP 3.65V");
    ASSERT_FLOAT_EQ(s.max_charge_a, 80.0f, "max charge 80A");
    ASSERT(s.charge_switch == true, "charge switch");
    ASSERT(s.balancer_switch == true, "balancer switch");
    ASSERT_FLOAT_EQ(s.nominal_ah, 200.0f, "nominal 200Ah");
    ASSERT_FLOAT_EQ(s.balance_trigger_v, 0.010f, "balance trigger 0.01V");
    ASSERT_FLOAT_EQ(s.start_balance_v, 3.000f, "start balance 3.0V");

    free(raw);
}

/* Test: assembler — feed fragments with AT\r\n prefix, verify frame reassembly. */
TEST(test_assembler_fragments_and_at_junk)
{
    uint8_t *raw = _make_cell_info_default();

    jk_frame_assembler_t asm_;
    jk_assembler_init(&asm_);

    uint8_t assembled[JK_FRAME_SIZE] = {0};

    /* First fragment: AT\r\n + 10 bytes of frame */
    uint8_t first[14];
    memcpy(first, "AT\r\n", 4);
    memcpy(first + 4, raw, 10);
    ASSERT(jk_assembler_feed(&asm_, first, 14, assembled, 1) == 0,
           "no frame yet from first fragment");

    /* Feed remaining 280 bytes in 20-byte chunks. */
    for (int i = 10; i < JK_FRAME_SIZE; i += 20) {
        if (jk_assembler_feed(&asm_, raw + i, 20, assembled, 1))
            break;
    }

    ASSERT(assembled[4] == JK_FRAME_CELL_INFO, "frame type");

    /* Decode the reassembled frame */
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(assembled, JK_FRAME_SIZE, 
                                          JK_PROTO_JK02_32S, 16, &info);
    ASSERT_OK(rc);
    ASSERT_FLOAT_EQ(info.soc_pct, 87, "SOC after reassembly");

    free(raw);
}

/* Test: assembler resyncs past junk bytes before header. */
TEST(test_assembler_resyncs_on_new_header)
{
    uint8_t *raw = _make_cell_info_default();

    jk_frame_assembler_t asm_;
    jk_assembler_init(&asm_);

    /* Feed junk + complete frame as a single chunk */
    uint8_t chunk[303];
    chunk[0] = 0x00; chunk[1] = 0x01; chunk[2] = 0x02;
    memcpy(chunk + 3, raw, JK_FRAME_SIZE);

    uint8_t assembled[JK_FRAME_SIZE] = {0};
    int got = jk_assembler_feed(&asm_, chunk, 303, assembled, 1);
    ASSERT(got >= 1, "frame assembled despite leading junk");
    ASSERT(assembled[4] == JK_FRAME_CELL_INFO, "correct frame type");

    free(raw);
}

/* Test: cell_info CRC at byte 299 is verified separately from command CRC. */
TEST(test_cell_info_crc_separate_from_command_crc)
{
    /* Verify: command CRC covers bytes 0..18, stored at byte 19 */
    const uint8_t *cmd = jk_build_command(JK_CMD_CELL_INFO, NULL, 0, 0);
    uint8_t cmd_crc = jk_crc8(cmd, 19);
    ASSERT(cmd[19] == cmd_crc, "command CRC at byte 19");

    /* Verify: cell-info CRC covers bytes 0..298, stored at byte 299 */
    uint8_t *raw = _make_cell_info_default();
    uint8_t frame_crc = jk_crc8(raw, 299);
    ASSERT(raw[299] == frame_crc, "cell-info CRC at byte 299");

    /* These are completely separate CRC calculations. */
    free(raw);
}

/* Test: 2 mV balancer heuristic. */
TEST(test_balancer_heuristic)
{
    uint16_t voltages[16];
    for (int i = 0; i < 16; i++) voltages[i] = 3300;
    voltages[2] = 3302;  /* 2 mV above baseline -> should be balancing */

    uint8_t *raw = _make_cell_info_frame(voltages, 16, 12400, 87, 42,
                                          0, 1, 1, 1, 800);
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_OK(rc);

    /* Cell at index 3 (voltages[2]) is 2 mV above min -> should be marked balancing */
    bool any_balancing = false;
    for (uint8_t i = 0; i < info.cell_count; i++) {
        if (info.cells[i].balancing) any_balancing = true;
    }
    ASSERT(any_balancing, "some cells marked balancing");

    free(raw);
}

/* Test: wrong frame type rejected by decoder. */
TEST(test_wrong_frame_type)
{
    uint8_t *raw = _make_cell_info_default();
    raw[4] = JK_FRAME_DEVICE_INFO;  /* change type */
    finish_crc(raw);

    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_ERR(rc);
    ASSERT(rc == JK_EBADTYPE, "wrong frame type rejected");

    free(raw);
}

/* Test: short frame rejected. */
TEST(test_short_frame)
{
    uint8_t small[10] = { 0x55, 0xAA, 0xEB, 0x90, 0x02 };
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(small, sizeof(small), JK_PROTO_JK02_32S, 16, &info);
    ASSERT_ERR(rc);
    ASSERT(rc == JK_EBADLEN, "short frame rejected");
}

/* Test: wrong header rejected. */
TEST(test_wrong_header)
{
    uint8_t *raw = _make_cell_info_default();
    raw[0] = 0xFF;  /* corrupt header */
    jk_cell_info_t info;
    jk_result_t rc = jk_decode_cell_info(raw, JK_FRAME_SIZE, JK_PROTO_JK02_32S, 16, &info);
    ASSERT_ERR(rc);
    ASSERT(rc == JK_EBADHDR, "wrong header rejected");

    free(raw);
}

/* ---- Main ---- */

int main(void)
{
    printf("Running JK protocol tests...\n");

    RUN_TEST(test_build_command_length_and_crc);
    RUN_TEST(test_build_command_counter);
    RUN_TEST(test_switch_commands);
    RUN_TEST(test_register_writes);
    RUN_TEST(test_decode_cell_info_16s);
    RUN_TEST(test_decode_cell_info_spread);
    RUN_TEST(test_crc_mismatch);
    RUN_TEST(test_implausible_pack_vs_cells);
    RUN_TEST(test_error_bits);
    RUN_TEST(test_decode_device_info);
    RUN_TEST(test_decode_settings);
    RUN_TEST(test_assembler_fragments_and_at_junk);
    RUN_TEST(test_assembler_resyncs_on_new_header);
    RUN_TEST(test_cell_info_crc_separate_from_command_crc);
    RUN_TEST(test_balancer_heuristic);
    RUN_TEST(test_wrong_frame_type);
    RUN_TEST(test_short_frame);
    RUN_TEST(test_wrong_header);

    printf("\n%d tests run, %d failed.\n", _tests_run, _tests_fail);
    return _tests_fail > 0 ? 1 : 0;
}
