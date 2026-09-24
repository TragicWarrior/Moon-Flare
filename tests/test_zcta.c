#define _GNU_SOURCE
#include "zcta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

/* A one-member .zip (local header + raw deflate), as census.gov serves. */
static unsigned char *make_zip(const char *txt, size_t *len)
{
    size_t tl = strlen(txt), cap = tl + 256;
    unsigned char *z = calloc(1, cap);
    z_stream zs;
    const char *name = "t.txt";
    size_t nl = strlen(name);

    memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = (unsigned char *)txt;
    zs.avail_in = (uInt)tl;
    zs.next_out = z + 30 + nl;
    zs.avail_out = (uInt)(cap - 30 - nl);
    deflate(&zs, Z_FINISH);
    z[0] = 0x50; z[1] = 0x4b; z[2] = 0x03; z[3] = 0x04;
    z[8] = 8;                                   /* deflate */
    z[18] = (unsigned char)zs.total_out;        /* sizes fit in 2 bytes here */
    z[19] = (unsigned char)(zs.total_out >> 8);
    z[22] = (unsigned char)tl;
    z[23] = (unsigned char)(tl >> 8);
    z[26] = (unsigned char)nl;
    memcpy(z + 30, name, nl);
    *len = 30 + nl + zs.total_out;
    deflateEnd(&zs);
    return z;
}

int main(void)
{
    const char *base = MF_SOURCE_DIR "/data/zcta.txt";
    const char *pipe_txt =
        "GEOID|GEOIDFQ|ALAND|AWATER|ALAND_SQMI|AWATER_SQMI|INTPTLAT|INTPTLONG\n"
        "00601|860Z200US00601|166744424|795116|64.38|0.307|18.180621|-66.749931\n"
        "75201|860Z200US75201|3564362|0|1.376|0.|32.788309|-96.799572\n";
    const char *tab_txt =
        "GEOID\tALAND\tAWATER\tALAND_SQMI\tAWATER_SQMI\tINTPTLAT\tINTPTLONG   \n"
        "00601\t166836392\t798613\t64.416\t0.308\t18.180555\t-66.749961      \n";
    char dir[] = "/tmp/mf-zcta-XXXXXX";
    char path[256], meta[256];
    char *out = NULL, *txt = NULL;
    size_t ol = 0, tl = 0, zl = 0;
    unsigned char *zip;
    double lat = 0, lon = 0;

    /* The shipped baseline. */
    CHECK(zcta_file_year(base) >= 2026, "baseline has a year header");
    CHECK(zcta_lookup(base, "75201", &lat, &lon) == 0 &&
          lat > 32.78 && lat < 32.80 && lon < -96.79 && lon > -96.81,
          "baseline: 75201 is Dallas");
    CHECK(zcta_lookup(base, "00000", &lat, &lon) == -1, "unknown ZIP");
    CHECK(zcta_lookup("/nonexistent", "75201", &lat, &lon) == -2,
          "missing table");

    /* Both Census layouts convert to our table. */
    CHECK(zcta_from_gazetteer(pipe_txt, strlen(pipe_txt), 2026, &out, &ol) == 2 &&
          strstr(out, "#moonflare-zcta 2026\n") == out &&
          strstr(out, "75201\t32.788309\t-96.799572\n"),
          "pipe-delimited gazetteer (2025+)");
    free(out);
    CHECK(zcta_from_gazetteer(tab_txt, strlen(tab_txt), 2024, &out, &ol) == 1 &&
          strstr(out, "00601\t18.180555\t-66.749961\n"),
          "tab-delimited gazetteer with trailing spaces (2024)");
    free(out);

    /* Unzip the single member, as a downloaded update would be. */
    zip = make_zip(pipe_txt, &zl);
    CHECK(zcta_unzip_single(zip, zl, &txt, &tl) == 0 && tl == strlen(pipe_txt) &&
          memcmp(txt, pipe_txt, tl) == 0, "unzip single member");
    free(txt);
    zip[0] = 'X';
    CHECK(zcta_unzip_single(zip, zl, &txt, &tl) == -1, "reject non-zip");
    free(zip);

    /* Install + pick + meta, as an update does. */
    if (!mkdtemp(dir))
        return 1;
    snprintf(path, sizeof(path), "%s/weathergov/zcta.txt", dir);
    snprintf(meta, sizeof(meta), "%s/weathergov/zcta.meta", dir);
    zcta_from_gazetteer(pipe_txt, strlen(pipe_txt), 2027, &out, &ol);
    CHECK(zcta_write_atomic(path, out, ol) == 0, "write table (creates dir)");
    free(out);
    CHECK(strcmp(zcta_pick(path, base), path) == 0, "state copy wins");
    CHECK(zcta_file_year(path) == 2027, "installed year");
    CHECK(zcta_meta_checked(meta) == 0, "no meta yet");
    CHECK(zcta_meta_write(meta, 1790000000L) == 0 &&
          zcta_meta_checked(meta) == 1790000000L, "meta round-trip");
    unlink(path);
    unlink(meta);
    snprintf(path, sizeof(path), "%s/weathergov", dir);
    rmdir(path);
    rmdir(dir);
    CHECK(strcmp(zcta_pick("/nonexistent/zcta.txt", base), base) == 0,
          "baseline when no state copy");

    if (g_fail)
        return 1;
    printf("test_zcta: ok\n");
    return 0;
}
