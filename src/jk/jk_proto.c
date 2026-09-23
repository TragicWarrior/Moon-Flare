#include "jk_proto.h"
#include <string.h>

/* ---- Constants ---- */

const uint8_t JK_HEADER_RSP[4] = { 0x55, 0xAA, 0xEB, 0x90 };
const uint8_t JK_HEADER_CMD[4] = { 0xAA, 0x55, 0x90, 0xEB };

static const uint8_t AT_JUNK[JK_AT_JUNK_SIZE] = { 'A', 'T', '\r', '\n' };

/* ---- CRC ---- */

uint8_t jk_crc8(const uint8_t *data, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++)
    {
        s += data[i];
    }
    return s;
}

/* ---- Command builders ---- */

static uint8_t _cmd_frame[JK_CMD_FRAME_SIZE];

const uint8_t *jk_build_command(uint8_t cmd,
                                const uint8_t *value, size_t vlen,
                                uint8_t counter)
{
    memset(_cmd_frame, 0, JK_CMD_FRAME_SIZE);
    memcpy(_cmd_frame, JK_HEADER_CMD, 4);
    _cmd_frame[4] = cmd;
    _cmd_frame[5] = (uint8_t)vlen;
    if (value && vlen > 0)
    {
        if (vlen > 13)
        {
            vlen = 13;
        }
        memcpy(_cmd_frame + 6, value, vlen);
    }
    else
    {
        _cmd_frame[16] = counter;
    }
    _cmd_frame[JK_CMD_CRC_OFFSET] = jk_crc8(_cmd_frame, JK_CMD_CRC_OFFSET);
    return _cmd_frame;
}

const uint8_t *jk_build_switch_cmd(uint8_t reg, bool on)
{
    uint8_t payload[4];
    memset(payload, 0, sizeof(payload));
    payload[0] = on ? 1 : 0;
    return jk_build_command(reg, payload, 4, 0);
}

const uint8_t *jk_build_register_cmd(uint8_t reg, uint32_t value)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(value);
    payload[1] = (uint8_t)(value >> 8);
    payload[2] = (uint8_t)(value >> 16);
    payload[3] = (uint8_t)(value >> 24);
    return jk_build_command(reg, payload, 4, 0);
}

static double round3(double v)
{
    if (v >= 0.0)
    {
        return (double)((long)(v * 1000.0 + 0.5)) / 1000.0;
    }
    return (double)((long)(v * 1000.0 - 0.5)) / 1000.0;
}

double jk_clamp_trigger_v(double volts)
{
    double v = round3(volts);
    if (v < JK_TRIGGER_MIN_V)
    {
        return JK_TRIGGER_MIN_V;
    }
    if (v > JK_TRIGGER_MAX_V)
    {
        return JK_TRIGGER_MAX_V;
    }
    return v;
}

double jk_clamp_start_v(double volts)
{
    double v = round3(volts);
    if (v < JK_START_MIN_V)
    {
        return JK_START_MIN_V;
    }
    if (v > JK_START_MAX_V)
    {
        return JK_START_MAX_V;
    }
    return v;
}

double jk_clamp_ovp_v(double volts)
{
    double v = round3(volts);
    if (v < JK_OVP_MIN_V)
    {
        return JK_OVP_MIN_V;
    }
    if (v > JK_OVP_MAX_V)
    {
        return JK_OVP_MAX_V;
    }
    return v;
}

double jk_clamp_ovpr_v(double volts, double ovp_v)
{
    double cap, v;

    ovp_v = jk_clamp_ovp_v(ovp_v);
    cap = round3(ovp_v - JK_OVPR_GAP_V);
        if (cap < JK_OVP_MIN_V - JK_OVPR_GAP_V)
        {
            cap = JK_OVP_MIN_V - JK_OVPR_GAP_V;
        }
    v = round3(volts);
    if (v > cap)
    {
        v = cap;
    }
    if (v < 2.40)
    {
        v = 2.40;
    }
    return v;
}

uint32_t jk_volts_to_mv(double volts)
{
    if (volts >= 0.0)
    {
        return (uint32_t)(volts * 1000.0 + 0.5);
    }
    return 0;
}

/* ---- Assembler ---- */

void jk_assembler_init(jk_frame_assembler_t *asm_)
{
    memset(asm_, 0, sizeof(*asm_));
}

int jk_assembler_feed(jk_frame_assembler_t *asm_,
                      const uint8_t *chunk, size_t chunk_len,
                      uint8_t *frame_buf, int max_frames)
{
    int n = 0;


    /* Strip leading AT\r\n */
    if (chunk_len >= JK_AT_JUNK_SIZE &&
        memcmp(chunk, AT_JUNK, JK_AT_JUNK_SIZE) == 0)
    {
        chunk += JK_AT_JUNK_SIZE;
        chunk_len -= JK_AT_JUNK_SIZE;
    }
    /* Strip trailing AT\r\n */
    if (chunk_len >= JK_AT_JUNK_SIZE &&
        memcmp(chunk + chunk_len - JK_AT_JUNK_SIZE,
               AT_JUNK, JK_AT_JUNK_SIZE) == 0)
    {
        chunk_len -= JK_AT_JUNK_SIZE;
    }
    if (chunk_len == 0 && asm_->buf_len < JK_FRAME_SIZE)
    {
        return 0;
    }

    /* Append to buffer */
    if (chunk_len > sizeof(asm_->buf) - asm_->buf_len)
    {
        size_t avail = sizeof(asm_->buf) - asm_->buf_len;
        memcpy(asm_->buf + asm_->buf_len, chunk, avail);
        asm_->buf_len += avail;
        asm_->dropped += (uint32_t)(chunk_len - avail);
        return n;
    }
    memcpy(asm_->buf + asm_->buf_len, chunk, chunk_len);
    asm_->buf_len += chunk_len;

    /* Drain complete frames */
    while (n < max_frames && asm_->buf_len >= 4)
    {
        /* Find header */
        size_t hpos = 0;
        while (hpos + 4 <= asm_->buf_len)
        {
            if (memcmp(asm_->buf + hpos, JK_HEADER_RSP, 4) == 0)
                break;
            hpos++;
        }

        if (hpos + 4 > asm_->buf_len)
        {
            /* Not enough bytes for any header */
            if (asm_->buf_len > 4)
            {
                asm_->dropped += (uint32_t)(asm_->buf_len - 4);
                memmove(asm_->buf, asm_->buf + asm_->buf_len - 4, 4);
                asm_->buf_len = 4;
            }
            break;
        }

        if (hpos > 0)
        {
            asm_->dropped += (uint32_t)(hpos);
            memmove(asm_->buf, asm_->buf + hpos, asm_->buf_len - hpos);
            asm_->buf_len -= hpos;
            continue;
        }

        /* Header at position 0; need full frame */
        if (asm_->buf_len < JK_FRAME_SIZE)
        {
            break;
        }

        const uint8_t *frame = asm_->buf;
        uint8_t crc = jk_crc8(frame, JK_FRAME_SIZE - 1);
        if (crc == frame[JK_CELL_CRC_OFFSET] &&
            (frame[4] == JK_FRAME_CELL_INFO ||
             frame[4] == JK_FRAME_SETTINGS ||
             frame[4] == JK_FRAME_DEVICE_INFO))
        {
            memcpy(frame_buf, frame, JK_FRAME_SIZE);
            n++;
            memmove(asm_->buf, frame + JK_FRAME_SIZE,
                    asm_->buf_len - JK_FRAME_SIZE);
            asm_->buf_len -= JK_FRAME_SIZE;
            if (n >= max_frames) break;
            continue;
        }

        /* Bad CRC or unknown type — search for next header */
        asm_->corrupt++;
        size_t next = 1;
        bool found = false;
        while (next + 3 < asm_->buf_len)
        {
            if (memcmp(asm_->buf + next, JK_HEADER_RSP, 4) == 0)
            {
                asm_->dropped += (uint32_t)(next);
                memmove(asm_->buf, asm_->buf + next,
                        asm_->buf_len - next);
                asm_->buf_len -= next;
                found = true;
                break;
            }
            next++;
        }
        if (!found)
        {
            if (asm_->buf_len > 4)
            {
                asm_->dropped += (uint32_t)(asm_->buf_len - 4);
                memmove(asm_->buf, asm_->buf + asm_->buf_len - 4, 4);
            }
            asm_->buf_len = (asm_->buf_len > 4) ? 4 : 0;
        }
    }

    return n;
}

/* ---- Decoders ---- */

jk_result_t jk_decode_cell_info(const uint8_t *data, size_t len,
                                jk_proto_t proto,
                                uint8_t expected_cells,
                                jk_cell_info_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Length check */
    if (!data || len < JK_FRAME_SIZE)
        return JK_EBADLEN;

    /* Header check */
    if (memcmp(data, JK_HEADER_RSP, 4) != 0)
        return JK_EBADHDR;

    /* Frame type check */
    if (data[4] != JK_FRAME_CELL_INFO)
        return JK_EBADTYPE;

    /* CRC check at byte 299 */
    if (jk_crc8(data, JK_FRAME_SIZE - 1) != data[JK_CELL_CRC_OFFSET])
        return JK_ECRC;

    out->raw = data;

    /* Offsets: JK02_32S uses 0, JK02_24S uses -32 */
    int off = (proto == JK_PROTO_JK02_24S) ? -32 : 0;

    uint8_t n_slots = (proto == JK_PROTO_JK02_32S) ? 32 : 24;

    /* Cell voltages: u16 LE mV at offset 6+i*2 */
    uint32_t enabled_mask = jk_u32(data, (size_t)(70 + off));
    out->enabled_mask = enabled_mask;

    for (uint8_t i = 0; i < n_slots; i++)
    {
        uint16_t mv = jk_u16(data, (size_t)(6 + i * 2));
        if (mv == 0)
            continue;

        jk_cell_t *c = &out->cells[out->cell_count];
        c->index = i + 1;
        c->voltage_v = mv * 0.001f;
        c->enabled = true;

        /* Cell resistance: u16 LE at 80+offset+i*2, scaled by 0.001 */
        size_t roff = (size_t)(80 + off + i * 2);
        if (roff + 2 <= JK_FRAME_SIZE)
        {
            c->resistance_ohm = jk_u16(data, roff) * 0.001f;
        }

        out->cell_count++;
    }

    /* Sanity: live cell count must not exceed slot count */
    if (out->cell_count > n_slots)
    {
        return JK_EIMPLAUSIBLE;
    }

    if (expected_cells && out->cell_count > expected_cells + 1)
    {
        return JK_EIMPLAUSIBLE;
    }

    /* Derived fields */
    out->average_cell_v = jk_u16(data, (size_t)(74 + off)) * 0.001f;
    out->delta_cell_v = jk_u16(data, (size_t)(76 + off)) * 0.001f;
    out->max_cell_number = data[78 + off];
    out->min_cell_number = data[79 + off];

    out->mosfet_temp_c = jk_temp_c(data, (size_t)(144 + off));
    out->wire_resistance_warnings = jk_u32(data, (size_t)(146 + off));
    out->pack_voltage_v = (float)jk_u32(data, (size_t)(150 + off)) * 0.001f;
    out->current_a = (float)jk_i32(data, (size_t)(158 + off)) * 0.001f;
    out->temp1_c = jk_temp_c(data, (size_t)(162 + off));
    out->temp2_c = jk_temp_c(data, (size_t)(164 + off));
    out->errors_bitmask = jk_u32(data, (size_t)(166 + off));
    out->balance_current_a = jk_i16(data, (size_t)(170 + off)) * 0.001f;
    out->balancing_action = (jk_balancing_action_t)data[172 + off];
    out->soc_pct = (float)data[173 + off];
    out->remaining_ah = (float)jk_u32(data, (size_t)(174 + off)) * 0.001f;
    out->nominal_ah = (float)jk_u32(data, (size_t)(178 + off)) * 0.001f;
    out->cycle_count = jk_u32(data, (size_t)(182 + off));
    out->cycle_capacity_ah = (float)jk_u32(data, (size_t)(186 + off)) * 0.001f;
    out->soh_pct = (float)data[190 + off];
    out->precharge_on = data[191 + off] ? true : false;
    out->runtime_s = jk_u32(data, (size_t)(194 + off));
    out->charge_mosfet_on = data[198 + off] ? true : false;
    out->discharge_mosfet_on = data[199 + off] ? true : false;
    out->precharging = data[200 + off] ? true : false;
    out->balancing_indicator = data[201 + off] ? true : false;

    size_t htoff = (size_t)(215 + off);
    out->heating_on = (htoff < JK_FRAME_SIZE) ?
                      (data[htoff] ? true : false) : false;

    size_t ecdoff = (size_t)(218 + off);
    out->emergency_countdown_s = (ecdoff + 2 <= JK_FRAME_SIZE) ?
                                  jk_u16(data, ecdoff) : 0;

    /* Extra temps for JK02_32S */
    if (proto == JK_PROTO_JK02_32S)
    {
        out->temp5_c = jk_temp_c(data, 254);
        out->temp4_c = jk_temp_c(data, 256);
        out->temp3_c = jk_temp_c(data, 258);
        if (JK_FRAME_SIZE > 280)
        {
            out->charge_status = data[280];
        }
    }

    /* Compute pack power */
    out->pack_power_w = out->pack_voltage_v * out->current_a;

    /* Plausibility: cell voltage range */
    for (uint8_t i = 0; i < out->cell_count; i++)
    {
        if (out->cells[i].voltage_v < 0.5f || out->cells[i].voltage_v > 5.0f)
        {
            return JK_EIMPLAUSIBLE;
        }
    }

    /* Plausibility: pack voltage vs cell sum */
    float cell_sum = 0.0f;
    for (uint8_t i = 0; i < out->cell_count; i++)
    {
        cell_sum += out->cells[i].voltage_v;
    }

    if (out->cell_count > 0 &&
        (out->pack_voltage_v < cell_sum - (0.15f * cell_sum + 1.0f) ||
         out->pack_voltage_v > cell_sum + (0.15f * cell_sum + 1.0f)))
    {
        return JK_EIMPLAUSIBLE;
    }

    /* Plausibility: current */
    if (out->current_a > 2000.0f || out->current_a < -2000.0f)
    {
        return JK_EIMPLAUSIBLE;
    }

    /* Plausibility: SOC */
    if (out->soc_pct > 100.0f)
    {
        return JK_EIMPLAUSIBLE;
    }

    /* 2 mV balancing heuristic */
    if (out->balancing_action != JK_BALANCE_OFF || out->balancing_indicator)
    {
        float max_v = 0.0f;
        for (uint8_t i = 0; i < out->cell_count; i++)
        {
            if (out->cells[i].voltage_v > max_v)
            {
                max_v = out->cells[i].voltage_v;
            }
        }
        for (uint8_t i = 0; i < out->cell_count; i++)
        {
            out->cells[i].balancing =
                (max_v - out->cells[i].voltage_v) <= 0.002f;
        }
    }

    return JK_OK;
}

static void _read_str(const uint8_t *data, size_t off, size_t len,
                      char *out, size_t max_len)
{
    if (off >= JK_FRAME_SIZE)
    {
        *out = '\0';
        return;
    }
    size_t n = len;
    if (off + n > JK_FRAME_SIZE) n = JK_FRAME_SIZE - off;
    for (size_t i = 0; i < n && data[off + i] != '\0'; i++)
    {
        out[i] = (char)data[off + i];
    }
    out[n] = '\0';
    /* Strip trailing whitespace */
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t' ||
                      out[n-1] == '\r' || out[n-1] == '\n'))
    {
        out[--n] = '\0';
    }
}

jk_result_t jk_decode_device_info(const uint8_t *data, size_t len,
                                  jk_device_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->raw = data;

    if (!data || len < JK_FRAME_SIZE)
        return JK_EBADLEN;
    if (memcmp(data, JK_HEADER_RSP, 4) != 0)
        return JK_EBADHDR;
    if (data[4] != JK_FRAME_DEVICE_INFO)
        return JK_EBADTYPE;
    if (jk_crc8(data, JK_FRAME_SIZE - 1) != data[JK_CELL_CRC_OFFSET])
        return JK_ECRC;

    _read_str(data, 6, 16, out->vendor, 17);
    _read_str(data, 22, 8, out->hardware_version, 9);
    _read_str(data, 30, 8, out->software_version, 9);
    out->uptime_s = jk_u32(data, 38);
    out->power_on_count = jk_u32(data, 42);
    _read_str(data, 46, 16, out->device_name, 17);
    _read_str(data, 62, 16, out->device_passcode, 17);
    _read_str(data, 78, 8, out->manufacturing_date, 9);
    _read_str(data, 86, 16, out->serial_number, 17);
    _read_str(data, 102, 16, out->user_data, 17);

    return JK_OK;
}

jk_result_t jk_decode_settings(const uint8_t *data, size_t len,
                               jk_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->raw = data;

    if (!data || len < JK_FRAME_SIZE)
        return JK_EBADLEN;
    if (memcmp(data, JK_HEADER_RSP, 4) != 0)
        return JK_EBADHDR;
    if (data[4] != JK_FRAME_SETTINGS)
        return JK_EBADTYPE;
    if (jk_crc8(data, JK_FRAME_SIZE - 1) != data[JK_CELL_CRC_OFFSET])
        return JK_ECRC;

    out->cell_uvp_v     = (float)jk_u32(data, 10) * 0.001f;
    out->cell_uvpr_v    = (float)jk_u32(data, 14) * 0.001f;
    out->cell_ovp_v     = (float)jk_u32(data, 18) * 0.001f;
    out->cell_ovpr_v    = (float)jk_u32(data, 22) * 0.001f;
    out->cell_rcv_v     = (float)jk_u32(data, 38) * 0.001f;
    out->balance_trigger_v = (float)jk_u32(data, 26) * 0.001f;
    out->start_balance_v   = (float)jk_u32(data, 138) * 0.001f;
    out->power_off_v      = (float)jk_u32(data, 46) * 0.001f;
    out->max_charge_a     = (float)jk_u32(data, 50) * 0.001f;
    out->max_discharge_a  = (float)jk_u32(data, 62) * 0.001f;
    out->max_balance_a    = (float)jk_u32(data, 78) * 0.001f;
    out->charge_otp_c     = (float)jk_u32(data, 82) * 0.1f;
    out->discharge_otp_c  = (float)jk_u32(data, 90) * 0.1f;
    out->charge_utp_c     = (float)jk_i32(data, 98) * 0.1f;
    out->power_tube_otp_c = (float)jk_i32(data, 106) * 0.1f;
    out->cell_count       = data[114];
    out->charge_switch    = data[118] ? true : false;
    out->discharge_switch = data[122] ? true : false;
    out->balancer_switch  = data[126] ? true : false;
    out->nominal_ah       = (float)jk_u32(data, 130) * 0.001f;

    return JK_OK;
}

/* ---- Helpers ---- */

void jk_cstring(const uint8_t *data, size_t offset, size_t len,
                char *out, size_t max_len)
{
    size_t n = len < max_len - 1 ? len : max_len - 1;
    size_t end = offset + n;
    if (end > 4096) end = 4096;  /* safety */
    for (size_t i = 0; i < n && data[offset + i] != '\0'; i++)
    {
        out[i] = (char)data[offset + i];
    }
    out[n] = '\0';
}

float jk_temp_c(const uint8_t *data, size_t off)
{
    int16_t raw = jk_i16(data, off);
    if (raw == JK_TEMP_ABSENT)
    {
        return (float)JK_TEMP_ABSENT;
    }
    return raw * 0.1f;
}
