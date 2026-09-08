#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "bms_proto.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* 16S / 5T realtime payload (86 bytes). */
static size_t recorded_0x01_payload(uint8_t *b, size_t cap)
{
    const int n = 16, t = 5;
    size_t base = (size_t)(2 * n + 2 * t);
    size_t need = 44 + base;
    int i;
    if (cap < need)
        return 0;
    memset(b, 0, need);
    b[1] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        uint16_t mv = (uint16_t)(3300 + i);
        uint8_t hi = (uint8_t)((mv >> 8) & 0x1F);
        uint8_t lo = (uint8_t)(mv & 0xFF);
        if (i == 3)
            hi = (uint8_t)(hi | 0x80);
        b[2 * i + 2] = hi;
        b[2 * i + 3] = lo;
    }
    put_be16(b + 4 + 2 * n, 28760);  /* 12.40 A */
    put_be16(b + 8 + 2 * n, 7600);   /* 76.00 % */
    put_be16(b + 12 + 2 * n, 20000); /* 200.00 Ah */
    b[15 + 2 * n] = (uint8_t)t;
    for (i = 0; i < t; i++)
        b[17 + 2 * n + 2 * i] = (uint8_t)(50 + 25 + i);
    put_be16(b + 30 + base, 42);
    put_be16(b + 34 + base, 5321); /* 53.21 V */
    put_be16(b + 38 + base, 9800); /* 98.00 % */
    return need;
}

int main(void)
{
    uint8_t frame[BMS_MAX_FRAME];
    uint8_t body[BMS_MAX_PAYLOAD];
    size_t flen, plen;
    bms_realtime_t r, r2;

    flen = bms_build_frame(1, 0x01, NULL, 0, frame);
    CHECK(flen == 6, "empty 0x01 frame is 6 bytes");
    CHECK(frame[0] == 0x7E && frame[1] == 0x01 && frame[2] == 0x01 &&
          frame[3] == 0x00 && frame[5] == 0x0D, "SOI/ADDR/CMD/LEN/EOI");
    CHECK(frame[4] == 0xFE, "checksum 0xFE for 7E 01 01 00");
    CHECK(bms_check(frame, 4) == 0xFE, "bms_check matches");

    plen = recorded_0x01_payload(body, sizeof(body));
    CHECK(plen == 86, "16S/5T payload 86 bytes");
    CHECK(bms_parse_realtime(body, plen, &r) == 0, "parse recorded 0x01");
    CHECK(r.cell_count == 16, "cell_count 16");
    CHECK(r.temp_count == 5, "temp_count 5");
    CHECK(r.cell_voltages_v[0] > 3.299 && r.cell_voltages_v[0] < 3.301,
          "cell0 3.300 V");
    CHECK(r.cell_balancing[3], "cell3 balancing flag");
    CHECK(!r.cell_balancing[0], "cell0 not balancing");
    CHECK(r.current_a > 12.39 && r.current_a < 12.41, "current 12.40 A");
    CHECK(r.soc_pct > 75.99 && r.soc_pct < 76.01, "soc 76.00");
    CHECK(r.pack_voltage_v > 53.20 && r.pack_voltage_v < 53.22, "pack 53.21 V");
    CHECK(r.soh_pct > 97.99 && r.soh_pct < 98.01, "soh 98.00");
    CHECK(r.cycles == 42, "cycles 42");
    CHECK(r.full_capacity_ah > 199.99 && r.full_capacity_ah < 200.01,
          "capacity 200 Ah");
    CHECK(r.temperatures_c[0] > 24.9 && r.temperatures_c[0] < 25.1, "T0 25 C");

    CHECK(bms_parse_realtime(body, plen, &r2) == 0, "second parse");
    CHECK(r2.pack_voltage_v == r.pack_voltage_v, "parse is stateless");

    CHECK(bms_parse_realtime(body, 2, &r) != 0, "short payload fails");

    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("xd_proto: ok\n");
    return 0;
}
