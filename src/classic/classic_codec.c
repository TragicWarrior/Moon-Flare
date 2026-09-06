#include "classic_codec.h"
#include <string.h>
#include <stdio.h>

classic_result_t classic_decode(const classic_registers_t *input,
                                classic_data_t *out)
{
    if (!input || !out || !input->regs)
        return CLASSIC_EBADREG;

    memset(out, 0, sizeof(*out));

    /* Helper: check bounds and read a wire-address-indexed register */
    #define R(addr) do { if ((addr) >= input->reg_count) return CLASSIC_EMISSING_REG; } while(0)
    #define U16(addr) ((addr) < input->reg_count ? input->regs[(addr)] : 0)
    #define I16_SIGNED(addr) ((int16_t)(uint16_t)U16((addr)))

    /* Type check: wire 4209 must be 0x4C43 ('LC') */
    R(CLASSIC_REG_CLASSIC_NAME);
    out->classic_name = U16(CLASSIC_REG_CLASSIC_NAME);
    if (out->classic_name != CLASSIC_EXPECTED_NAME)
        return CLASSIC_ETYPE_MISMATCH;

    /* Battery voltage: wire 4114, u16 /10 V */
    R(CLASSIC_REG_BATT_VOLTAGE);
    out->battery_voltage_v = U16(CLASSIC_REG_BATT_VOLTAGE) * 0.1f;

    /* Ibatt: wire 4116, i16 /10 A (signed) */
    R(CLASSIC_REG_IBATT);
    out->ibatt_raw = I16_SIGNED(CLASSIC_REG_IBATT);
    out->ibatt_a = out->ibatt_raw * 0.1f;

    /* kWh today: wire 4117, u16 /10 kWh */
    R(CLASSIC_REG_KWH_TODAY);
    out->kwh_today = U16(CLASSIC_REG_KWH_TODAY) * 0.1f;

    /* Watts: wire 4118, u16 unscaled */
    R(CLASSIC_REG_WATTS);
    out->watts = U16(CLASSIC_REG_WATTS);

    /* Charge stage: wire 4119, MSB = stage code */
    R(CLASSIC_REG_CHARGE_STAGE);
    out->charge_stage_msb = (U16(CLASSIC_REG_CHARGE_STAGE) >> 8) & 0xFF;
    out->charge_stage_name[0] = '\0';
    const char *stage_str = classic_charge_stage_name(out->charge_stage_msb);
    strncpy(out->charge_stage_name, stage_str, sizeof(out->charge_stage_name) - 1);
    out->charge_stage_name[sizeof(out->charge_stage_name) - 1] = '\0';
    switch (out->charge_stage_msb) {
        case 0:  out->charge_stage = CLASSIC_STAGE_RESTING;      break;
        case 3:  out->charge_stage = CLASSIC_STAGE_ABSORB;       break;
        case 4:  out->charge_stage = CLASSIC_STAGE_BULK_MPPT;    break;
        case 5:  out->charge_stage = CLASSIC_STAGE_FLOAT;        break;
        case 6:  out->charge_stage = CLASSIC_STAGE_FLOAT_MPPT;   break;
        case 7:  out->charge_stage = CLASSIC_STAGE_EQUALIZE;     break;
        case 10: out->charge_stage = CLASSIC_STAGE_HYPERVOC;     break;
        case 18: out->charge_stage = CLASSIC_STAGE_EQMPPT;       break;
        default: out->charge_stage = (classic_charge_stage_t)out->charge_stage_msb; break;
    }

    /* Ah today: wire 4124, u16 /1 Ah */
    R(CLASSIC_REG_AH_TODAY);
    out->ah_today = U16(CLASSIC_REG_AH_TODAY);

    /* Lifetime kWh: wire[4126]<<16 | wire[4125] (low register first) */
    R(CLASSIC_REG_LIFETIME_KWH_LO);
    R(CLASSIC_REG_LIFETIME_KWH_HI);
    out->lifetime_kwh = ((uint32_t)U16(CLASSIC_REG_LIFETIME_KWH_HI) << 16)
                      | (uint32_t)U16(CLASSIC_REG_LIFETIME_KWH_LO);

    /* UNIT_MAC: wire 4105-4107, 6 bytes */
    R(CLASSIC_REG_UNIT_MAC_LO);
    R(CLASSIC_REG_UNIT_MAC_MID);
    R(CLASSIC_REG_UNIT_MAC_HI);
    out->unit_mac[0] = (U16(CLASSIC_REG_UNIT_MAC_HI)     >> 8) & 0xFF;
    out->unit_mac[1] =  U16(CLASSIC_REG_UNIT_MAC_HI)      & 0xFF;
    out->unit_mac[2] = (U16(CLASSIC_REG_UNIT_MAC_MID)    >> 8) & 0xFF;
    out->unit_mac[3] =  U16(CLASSIC_REG_UNIT_MAC_MID)      & 0xFF;
    out->unit_mac[4] = (U16(CLASSIC_REG_UNIT_MAC_LO)     >> 8) & 0xFF;
    out->unit_mac[5] =  U16(CLASSIC_REG_UNIT_MAC_LO)       & 0xFF;

    /* UNIT_Device_ID: wire[4111]<<16 | wire[4110] */
    R(CLASSIC_REG_DEVICE_ID_LO);
    R(CLASSIC_REG_DEVICE_ID_HI);
    out->device_id = ((uint32_t)U16(CLASSIC_REG_DEVICE_ID_HI) << 16)
                   | (uint32_t)U16(CLASSIC_REG_DEVICE_ID_LO);

    return CLASSIC_OK;
}
