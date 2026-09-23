#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "classic_codec.h"

#define TOL_F 0.01f

static int _tests_run = 0;
static int _tests_fail = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(fn) do { _tests_run++; fn(); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { _tests_fail++; printf("FAIL: %s (line %d): %s\n", __FUNCTION__, __LINE__, msg); return; } } while(0)
#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_FLOAT_EQ(a, b, msg) ASSERT(((a) > (b) - TOL_F) && ((a) < (b) + TOL_F), msg)

static void make_golden_data(uint16_t *regs)
{
    memset(regs, 0, 4220 * sizeof(uint16_t));
    regs[4105] = 0x1122;  /* MAC lo */
    regs[4106] = 0x3344;  /* MAC mid */
    regs[4107] = 0x5566;  /* MAC hi */
    regs[4110] = 0xAABB;  /* Device ID lo */
    regs[4111] = 0xCCDD;  /* Device ID hi */
    regs[4114] = 1275;    /* Battery voltage: 127.5V /10 */
    regs[4116] = (uint16_t)(int16_t)(-155);  /* Ibatt: -15.5A /10 */
    regs[4117] = 34;      /* kWh today: 3.4 /10 */
    regs[4118] = 2500;    /* Watts: 2500 unscaled */
    regs[4119] = 0x0305;  /* Charge stage: MSB=3=Absorb, LSB=5 */
    regs[4124] = 452;     /* Ah today: 452 /1 */
    regs[4125] = 0x0001;  /* Lifetime kWh lo */
    regs[4126] = 0x0200;  /* Lifetime kWh hi -> 0x02000001 = 33554433 */
    regs[4209] = 0x4C43;  /* Classic name: 'LC' */
}

TEST(test_decode_golden)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    make_golden_data(regs);

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "golden decode succeeds");
    ASSERT_FLOAT_EQ(data.battery_voltage_v, 127.5f, "battery_voltage_v 127.5");
    ASSERT_EQ(data.ibatt_raw, -155, "ibatt_raw -155");
    ASSERT_FLOAT_EQ(data.ibatt_a, -15.5f, "ibatt_a -15.5");
    ASSERT_FLOAT_EQ(data.kwh_today, 3.4f, "kwh_today 3.4");
    ASSERT_EQ(data.watts, 2500, "watts 2500");
    ASSERT_EQ(data.charge_stage_msb, 3, "charge_stage_msb 3");
    ASSERT_EQ(data.charge_stage, CLASSIC_STAGE_ABSORB, "charge_stage Absorb");
    ASSERT(strcmp(data.charge_stage_name, "Absorb") == 0, "charge_stage_name Absorb");
    ASSERT_FLOAT_EQ(data.ah_today, 452.0f, "ah_today 452");
    ASSERT_EQ(data.lifetime_kwh, 0x02000001U, "lifetime_kwh 0x02000001");
    ASSERT_EQ(data.unit_mac[0], 0x55, "unit_mac[0] 0x55");
    ASSERT_EQ(data.unit_mac[1], 0x66, "unit_mac[1] 0x66");
    ASSERT_EQ(data.unit_mac[2], 0x33, "unit_mac[2] 0x33");
    ASSERT_EQ(data.unit_mac[3], 0x44, "unit_mac[3] 0x44");
    ASSERT_EQ(data.unit_mac[4], 0x11, "unit_mac[4] 0x11");
    ASSERT_EQ(data.unit_mac[5], 0x22, "unit_mac[5] 0x22");
    ASSERT_EQ(data.device_id, 0xCCDDAABB, "device_id 0xCCDDAABB");
    ASSERT_EQ(data.classic_name, 0x4C43, "classic_name 0x4C43");
}

TEST(test_type_check_mismatch)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    make_golden_data(regs);
    regs[4209] = 0xDEAD;  /* Wrong type code */

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_ETYPE_MISMATCH, rc, "type mismatch returns CLASSIC_ETYPE_MISMATCH");
    ASSERT_EQ(data.classic_name, 0xDEAD, "classic_name still populated");
}

TEST(test_negative_ibatt)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4114] = 1200;   /* 120.0V */
    regs[4116] = (uint16_t)(int16_t)(-2500);  /* -250.0A (max discharge) */
    regs[4117] = 0;
    regs[4118] = 0;
    regs[4119] = 0;      /* Resting */
    regs[4124] = 0;
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "negative Ibatt decodes OK");
    ASSERT_EQ(data.ibatt_raw, -2500, "ibatt_raw -2500");
    ASSERT_FLOAT_EQ(data.ibatt_a, -250.0f, "ibatt_a -250.0");
    ASSERT_EQ(data.charge_stage, CLASSIC_STAGE_RESTING, "charge_stage Resting");
    ASSERT(strcmp(data.charge_stage_name, "Resting") == 0, "charge_stage_name Resting");
}

TEST(test_lifetime_kwh_order)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4125] = 0x1234;  /* lo */
    regs[4126] = 0x5678;  /* hi -> 0x56781234 */
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "lifetime kWh decode OK");
    ASSERT_EQ(data.lifetime_kwh, 0x56781234U, "lifetime_kwh 0x56781234");
}

TEST(test_lifetime_kwh_swapped_fails)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4125] = 0x5678;  /* swapped: lo has hi value */
    regs[4126] = 0x1234;  /* swapped: hi has lo value */
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "decode still succeeds (no corruption)");
    ASSERT_EQ(data.lifetime_kwh, 0x12345678U, "lifetime_kwh 0x12345678 (swapped)");

    /* Verify the swapped value differs from the correct order */
    ASSERT(data.lifetime_kwh != 0x56781234U, "swapped lifetime_kwh differs from correct order");
}

TEST(test_pdf_as_wire_address_fails)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    /* Populate PDF register addresses (wire + 1) instead of wire addresses */
    regs[4115] = 1275;    /* PDF 4115 instead of wire 4114 */
    regs[4118] = 34;      /* PDF 4118 instead of wire 4117 */
    regs[4119] = 2500;    /* PDF 4119 instead of wire 4118 */
    regs[4120] = 0x0305;  /* PDF 4120 instead of wire 4119 */
    regs[4125] = 452;     /* PDF 4125 instead of wire 4124 */
    regs[4126] = 0x0001;  /* PDF 4126 instead of wire 4125 */
    regs[4127] = 0x0200;  /* PDF 4127 instead of wire 4126 */
    regs[4111] = 0xCCDD;  /* PDF 4112 instead of wire 4111 */
    regs[4112] = 0xAABB;  /* PDF 4113 instead of wire 4110 */
    regs[4106] = 0x1122;  /* PDF 4107 instead of wire 4105 */
    regs[4107] = 0x3344;  /* PDF 4108 instead of wire 4106 */
    regs[4108] = 0x5566;  /* PDF 4109 instead of wire 4107 */
    regs[4210] = 0x4C43;  /* PDF 4210 instead of wire 4209 */

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_ETYPE_MISMATCH, rc, "PDF addresses used as wire indices fails type check");
}

TEST(test_charge_stages)
{
    static const struct { uint8_t msb; classic_charge_stage_t stage; const char *name; } stages[] = {
        { 0,  CLASSIC_STAGE_RESTING,      "Resting" },
        { 3,  CLASSIC_STAGE_ABSORB,       "Absorb" },
        { 4,  CLASSIC_STAGE_BULK_MPPT,    "BulkMppt" },
        { 5,  CLASSIC_STAGE_FLOAT,        "Float" },
        { 6,  CLASSIC_STAGE_FLOAT_MPPT,   "FloatMppt" },
        { 7,  CLASSIC_STAGE_EQUALIZE,     "Equalize" },
        { 10, CLASSIC_STAGE_HYPERVOC,     "HyperVoc" },
        { 18, CLASSIC_STAGE_EQMPPT,       "EqMppt" },
    };

    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
    {
        uint16_t regs[4220];
        memset(regs, 0, sizeof(regs));
        regs[4119] = (uint16_t)stages[i].msb << 8;
        regs[4209] = 0x4C43;

        classic_registers_t input = { regs, 4220 };
        classic_data_t data;
        classic_result_t rc = classic_decode(&input, &data);

        ASSERT_EQ(CLASSIC_OK, rc, "decode OK for stage");
        ASSERT_EQ(data.charge_stage_msb, stages[i].msb, "charge_stage_msb");
        ASSERT_EQ(data.charge_stage, stages[i].stage, "charge_stage enum");
        ASSERT(strcmp(data.charge_stage_name, stages[i].name) == 0, "charge_stage_name");
    }
}

TEST(test_unknown_charge_stage)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4119] = 0x0900;  /* MSB = 9 (unknown) */
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "unknown stage decodes OK");
    ASSERT_EQ(data.charge_stage_msb, 9, "charge_stage_msb 9");
    ASSERT(strncmp(data.charge_stage_name, "Unknown", 7) == 0, "charge_stage_name Unknown(9)");
}

TEST(test_missing_register)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4115 };  /* reg_count too small for wire 4114 */
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_EMISSING_REG, rc, "missing register returns CLASSIC_EMISSING_REG");
}

TEST(test_null_input)
{
    classic_data_t data;
    ASSERT_EQ(CLASSIC_EBADREG, classic_decode(NULL, &data), "NULL input");
    ASSERT_EQ(CLASSIC_EBADREG, classic_decode(NULL, NULL), "NULL both");
}

TEST(test_all_zero_registers)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "zero registers decode OK");
    ASSERT_FLOAT_EQ(data.battery_voltage_v, 0.0f, "battery_voltage_v 0");
    ASSERT_FLOAT_EQ(data.ibatt_a, 0.0f, "ibatt_a 0");
    ASSERT_FLOAT_EQ(data.kwh_today, 0.0f, "kwh_today 0");
    ASSERT_EQ(data.watts, 0, "watts 0");
    ASSERT_FLOAT_EQ(data.ah_today, 0.0f, "ah_today 0");
    ASSERT_EQ(data.lifetime_kwh, 0U, "lifetime_kwh 0");
    ASSERT_EQ(data.device_id, 0U, "device_id 0");
}

TEST(test_unit_mac_order)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4105] = 0x0102;  /* MAC lo */
    regs[4106] = 0x0304;  /* MAC mid */
    regs[4107] = 0x0506;  /* MAC hi */
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "MAC decode OK");
    ASSERT_EQ(data.unit_mac[0], 0x05, "MAC[0] hi MSB");
    ASSERT_EQ(data.unit_mac[1], 0x06, "MAC[1] hi LSB");
    ASSERT_EQ(data.unit_mac[2], 0x03, "MAC[2] mid MSB");
    ASSERT_EQ(data.unit_mac[3], 0x04, "MAC[3] mid LSB");
    ASSERT_EQ(data.unit_mac[4], 0x01, "MAC[4] lo MSB");
    ASSERT_EQ(data.unit_mac[5], 0x02, "MAC[5] lo LSB");
}

TEST(test_device_id_order)
{
    uint16_t regs[4220];
    memset(regs, 0, sizeof(regs));
    regs[4110] = 0x0011;  /* Device ID lo */
    regs[4111] = 0x2233;  /* Device ID hi */
    regs[4209] = 0x4C43;

    classic_registers_t input = { regs, 4220 };
    classic_data_t data;
    classic_result_t rc = classic_decode(&input, &data);

    ASSERT_EQ(CLASSIC_OK, rc, "device ID decode OK");
    ASSERT_EQ(data.device_id, 0x22330011U, "device_id 0x22330011");
}

/* ---- Main ---- */

int main(void)
{
    printf("Running Classic 150 codec tests...\n");

    RUN_TEST(test_decode_golden);
    RUN_TEST(test_type_check_mismatch);
    RUN_TEST(test_negative_ibatt);
    RUN_TEST(test_lifetime_kwh_order);
    RUN_TEST(test_lifetime_kwh_swapped_fails);
    RUN_TEST(test_pdf_as_wire_address_fails);
    RUN_TEST(test_charge_stages);
    RUN_TEST(test_unknown_charge_stage);
    RUN_TEST(test_missing_register);
    RUN_TEST(test_null_input);
    RUN_TEST(test_all_zero_registers);
    RUN_TEST(test_unit_mac_order);
    RUN_TEST(test_device_id_order);

    printf("\n%d tests run, %d failed.\n", _tests_run, _tests_fail);
    return _tests_fail > 0 ? 1 : 0;
}
