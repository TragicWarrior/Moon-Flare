#ifndef CLASSIC_CODEC_H
#define CLASSIC_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Wire addresses (0-based: PDF reg - 1) */
#define CLASSIC_REG_UNIT_MAC_LO          4105
#define CLASSIC_REG_UNIT_MAC_MID         4106
#define CLASSIC_REG_UNIT_MAC_HI          4107
#define CLASSIC_REG_BATT_VOLTAGE         4114
#define CLASSIC_REG_IBATT                4116
#define CLASSIC_REG_KWH_TODAY            4117
#define CLASSIC_REG_WATTS                4118
#define CLASSIC_REG_CHARGE_STAGE         4119
#define CLASSIC_REG_AH_TODAY             4124
#define CLASSIC_REG_LIFETIME_KWH_LO      4125
#define CLASSIC_REG_LIFETIME_KWH_HI      4126
#define CLASSIC_REG_DEVICE_ID_LO         4110
#define CLASSIC_REG_DEVICE_ID_HI         4111
#define CLASSIC_REG_CLASSIC_NAME         4209

/* Type check value ('LC' in ASCII, big-endian) */
#define CLASSIC_EXPECTED_NAME            0x4C43

typedef enum {
    CLASSIC_OK             =  0,
    CLASSIC_EBADLEN        = -1,
    CLASSIC_ETYPE_MISMATCH = -2,
    CLASSIC_EMISSING_REG   = -3,
    CLASSIC_EBADREG        = -4
} classic_result_t;

typedef enum {
    CLASSIC_STAGE_RESTING      = 0,
    CLASSIC_STAGE_ABSORB       = 3,
    CLASSIC_STAGE_BULK_MPPT    = 4,
    CLASSIC_STAGE_FLOAT        = 5,
    CLASSIC_STAGE_FLOAT_MPPT   = 6,
    CLASSIC_STAGE_EQUALIZE     = 7,
    CLASSIC_STAGE_HYPERVOC     = 10,
    CLASSIC_STAGE_EQMPPT       = 18
} classic_charge_stage_t;

typedef struct {
    /* Battery */
    float    battery_voltage_v;   /* /10 V */
    int      ibatt_raw;           /* raw i16 */
    float    ibatt_a;             /* /10 A, signed */

    /* Energy / power */
    float    kwh_today;           /* /10 kWh */
    uint16_t watts;               /* unscaled */
    float    ah_today;            /* 1 Ah per count */
    uint32_t lifetime_kwh;        /* wire[4126]<<16 | wire[4125] */

    /* Charger state */
    classic_charge_stage_t charge_stage;
    char     charge_stage_name[16];
    uint8_t  charge_stage_msb;    /* raw MSB for debugging */

    /* Identity */
    uint8_t  unit_mac[6];         /* [4108]MSB:[4107]MSB:[4106]MSB */
    uint32_t device_id;           /* wire[4111]<<16 | wire[4110] */
    uint16_t classic_name;        /* wire 4209 value (for checking) */
} classic_data_t;

typedef struct {
    const uint16_t *regs;
    size_t           reg_count;
} classic_registers_t;

#define CLASSIC_CHARGE_STAGE_TABLE \
    X(0,   "Resting")      \
    X(3,   "Absorb")       \
    X(4,   "BulkMppt")     \
    X(5,   "Float")        \
    X(6,   "FloatMppt")    \
    X(7,   "Equalize")     \
    X(10,  "HyperVoc")     \
    X(18,  "EqMppt")

static inline const char *classic_charge_stage_name(uint8_t msb)
{
    switch (msb) {
        case 0:  return "Resting";
        case 3:  return "Absorb";
        case 4:  return "BulkMppt";
        case 5:  return "Float";
        case 6:  return "FloatMppt";
        case 7:  return "Equalize";
        case 10: return "HyperVoc";
        case 18: return "EqMppt";
        default: {
            static char buf[16];
            snprintf(buf, sizeof(buf), "Unknown(%u)", msb);
            return buf;
        }
    }
}

/* Decode all Classic 150 fields from wire-address-indexed register images.
 *
 * regs: array where regs[addr] gives the uint16_t image at wire address addr.
 * reg_count: size of the regs array (must be > max wire address accessed).
 * out: output buffer for decoded data (caller-allocated).
 *
 * Returns CLASSIC_OK on success, or error code:
 *   CLASSIC_ETYPE_MISMATCH  - wire 4209 != 0x4C43
 *   CLASSIC_EMISSING_REG    - requested wire address >= reg_count
 *   CLASSIC_EBADREG         - invalid parameters
 */
classic_result_t classic_decode(const classic_registers_t *input,
                                classic_data_t *out);

#endif /* CLASSIC_CODEC_H */
