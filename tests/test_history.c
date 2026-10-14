#define _GNU_SOURCE

#include "history.h"
#include <cJSON.h>

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

#define UUID_A "aaaaaaaa-0000-4000-8000-000000000001"
#define UUID_B "bbbbbbbb-0000-4000-8000-000000000002"
#define UUID_C "cccccccc-0000-4000-8000-000000000003"

static const char *k_bat_spec =
    "{\"interval_s\":10,\"min_s\":1,\"retention_days\":365,\"graph\":\"soc\",\"columns\":{"
    "\"pack_v\":\"pack_voltage_v\",\"current_a\":\"current_a\","
    "\"power_w\":\"power_w\",\"soc\":\"soc_pct\"}}";

static const char *k_wx_spec =
    "{\"interval_s\":600,\"min_s\":60,\"retention_days\":0,\"graph\":\"temp_f\",\"columns\":{"
    "\"temp_f\":\"weather.temp_f\","
    "\"conditions\":{\"path\":\"weather.conditions\",\"type\":\"text\"}}}";

static void tmpdir(char *path, size_t n)
{
    snprintf(path, n, "/tmp/mfhistXXXXXX");
    if (!mkdtemp(path))
        path[0] = '\0';
}

static void rmtree(const char *dir)
{
    char cmd[300];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warn: cleanup of %s failed\n", dir);
}

static int count_sql(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int cnt = -1;

    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        cnt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return cnt;
}

static double real_sql(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    double v = -99999.0;

    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return v;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void text_sql(sqlite3 *db, const char *sql, char *out, size_t cap)
{
    sqlite3_stmt *st = NULL;

    out[0] = '\0';
    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        snprintf(out, cap, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

static void sample(const char *uuid, double ts, double every, const char *reading)
{
    mf_sample_t s;

    memset(&s, 0, sizeof(s));
    snprintf(s.uuid, sizeof(s.uuid), "%s", uuid);
    s.ts = ts;
    s.online = 1;
    s.capture_interval_s = every;
    snprintf(s.reading, sizeof(s.reading), "%s", reading);
    mf_history_enqueue(&s);
}

static int file_exists(const char *dir, const char *name)
{
    char p[320];
    struct stat sb;

    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return stat(p, &sb) == 0;
}

/* Declared columns come out of the reading; the whole reading is kept. */
static void test_columns(void)
{
    char dir[64], buf[256];
    sqlite3 *db;
    double now = 1700000000.0;

    tmpdir(dir, sizeof(dir));
    CHECK(mf_history_open(dir) == 0, "open");
    CHECK(mf_history_is_open(), "is_open");
    CHECK(mf_history_register(UUID_A, "XD", "battery", "xd", k_bat_spec) == 0,
          "register battery");
    CHECK(mf_history_register(UUID_B, "Weather", "service", "weathergov",
                              k_wx_spec) == 0, "register weather");
    CHECK(file_exists(dir, UUID_A ".sqlite"), "battery file");
    CHECK(file_exists(dir, UUID_B ".sqlite"), "weather file");

    sample(UUID_A, now, 1.0,
           "{\"pack_voltage_v\":52.5,\"current_a\":-2.0,\"power_w\":-105,"
           "\"soc_pct\":80.5,\"cells\":[3.3,3.3]}");
    sample(UUID_B, now, 600.0,
           "{\"weather\":{\"temp_f\":71.6,\"conditions\":\"Haze\",\"icon\":\"fog\"}}");
    CHECK(mf_history_flush_slice(32) == 2, "flush 2");

    db = mf_history_module_db(UUID_A);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 1, "battery row");
    CHECK(real_sql(db, "SELECT soc FROM samples") == 80.5, "soc column");
    CHECK(real_sql(db, "SELECT power_w FROM samples") == -105.0, "power column");
    text_sql(db, "SELECT reading FROM samples", buf, sizeof(buf));
    CHECK(strstr(buf, "\"cells\"") != NULL, "reading kept whole");
    text_sql(db, "SELECT value FROM module WHERE key='kind'", buf, sizeof(buf));
    CHECK(strcmp(buf, "battery") == 0, "meta kind");

    db = mf_history_module_db(UUID_B);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 1, "weather row");
    CHECK(real_sql(db, "SELECT temp_f FROM samples") == 71.6, "nested numeric");
    text_sql(db, "SELECT conditions FROM samples", buf, sizeof(buf));
    CHECK(strcmp(buf, "Haze") == 0, "text column");
    /* Namespaces: one module's file has none of the other's columns. */
    CHECK(count_sql(db, "SELECT COUNT(*) FROM pragma_table_info('samples')"
                        " WHERE name='soc'") == 0, "no foreign columns");

    CHECK(strcmp(mf_history_graph_column(UUID_A), "soc") == 0, "graph soc");
    CHECK(strcmp(mf_history_graph_column(UUID_B), "temp_f") == 0, "graph temp");
    mf_history_close();
    rmtree(dir);
}

/* Interval downsampling, and modules that do not capture. */
static void test_capture_gate(void)
{
    char dir[64];
    double now = 1700000000.0;
    int i;

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_A, "XD", "battery", "xd", k_bat_spec);
    mf_history_register(UUID_C, "Plain", "inverter", "x", NULL);
    for (i = 0; i < 10; i++)
    {
        sample(UUID_A, now + i, 3.0, "{\"soc_pct\":50}");
        sample(UUID_C, now + i, 1.0, "{\"x\":1}");
        sample(UUID_A, now + i, 0.0, "{\"soc_pct\":50}"); /* off: dropped */
    }
    mf_history_flush_slice(1024);
    CHECK(count_sql(mf_history_module_db(UUID_A),
                    "SELECT COUNT(*) FROM samples") == 4, "every 3 s of 10");
    CHECK(count_sql(mf_history_module_db(UUID_C),
                    "SELECT COUNT(*) FROM samples") == 0, "no capture spec");
    CHECK(mf_history_graph_column(UUID_C) == NULL, "no graph");
    /* Unknown modules and unsafe ids are refused. */
    CHECK(mf_history_register("../etc/passwd", "x", "x", "x", k_bat_spec) != 0,
          "unsafe uuid");
    {
        mf_sample_t s;

        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "%s", UUID_B);
        s.capture_interval_s = 1.0;
        CHECK(mf_history_enqueue(&s) == -1, "unregistered enqueue");
    }
    mf_history_close();
    rmtree(dir);
}

/* A newer plugin declaring more columns grows the existing file. */
static void test_schema_evolution(void)
{
    char dir[64];
    sqlite3 *db;
    double now = 1700000000.0;
    const char *v1 = "{\"interval_s\":1,\"retention_days\":0,"
                     "\"columns\":{\"soc\":\"soc_pct\"}}";
    const char *v2 = "{\"interval_s\":1,\"retention_days\":0,\"graph\":\"temp_c\",\"columns\":{"
                     "\"soc\":\"soc_pct\",\"temp_c\":\"temp_c\"}}";

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_A, "XD", "battery", "xd", v1);
    sample(UUID_A, now, 1.0, "{\"soc_pct\":40,\"temp_c\":20}");
    mf_history_flush_slice(8);
    mf_history_close();

    mf_history_open(dir);
    CHECK(mf_history_register(UUID_A, "XD", "battery", "xd", v2) == 0,
          "re-register v2");
    sample(UUID_A, now + 5, 1.0, "{\"soc_pct\":41,\"temp_c\":21}");
    mf_history_flush_slice(8);
    db = mf_history_module_db(UUID_A);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 2, "both rows");
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples WHERE temp_c IS NULL") == 1,
          "old row has no new column value");
    CHECK(real_sql(db, "SELECT temp_c FROM samples WHERE soc = 41") == 21.0,
          "new column filled");
    /* Old readings are kept whole, so the new column can be backfilled. */
    CHECK(real_sql(db, "SELECT json_extract(reading,'$.temp_c') FROM samples"
                       " WHERE soc = 40") == 20.0, "backfillable");
    mf_history_close();
    rmtree(dir);
}

static void test_retire(void)
{
    char dir[64], buf[64];
    sqlite3 *db = NULL;

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_A, "XD", "battery", "xd", k_bat_spec);
    sample(UUID_A, 1700000000.0, 1.0, "{\"soc_pct\":1}");
    CHECK(mf_history_retire(UUID_A, 1700000100.0) == 0, "retire");
    CHECK(mf_history_module_db(UUID_A) == NULL, "closed");
    CHECK(file_exists(dir, UUID_A ".sqlite"), "file kept");
    {
        char p[320];

        snprintf(p, sizeof(p), "%s/" UUID_A ".sqlite", dir);
        sqlite3_open(p, &db);
        CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 1,
              "queued sample flushed before retire");
        text_sql(db, "SELECT value FROM module WHERE key='retired_ts'",
                 buf, sizeof(buf));
        CHECK(atof(buf) == 1700000100.0, "retired_ts");
        sqlite3_close(db);
    }
    /* Re-adding the module clears it. */
    mf_history_register(UUID_A, "XD", "battery", "xd", k_bat_spec);
    text_sql(mf_history_module_db(UUID_A),
             "SELECT value FROM module WHERE key='retired_ts'", buf, sizeof(buf));
    CHECK(buf[0] == '\0', "retired_ts cleared");
    mf_history_close();
    rmtree(dir);
}

static void test_query_step(void)
{
    char dir[64];
    double ts[64], v[64];
    double base = 1699999980.0;     /* a multiple of 60 */
    int i, n;

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_B, "Weather", "service", "weathergov", k_wx_spec);
    for (i = 0; i < 6; i++)          /* 0,20,..,100 s: bins 0 and 60 */
    {
        char r[96];

        snprintf(r, sizeof(r), "{\"weather\":{\"temp_f\":%d,\"conditions\":\"x\"}}",
                 i * 10);
        sample(UUID_B, base + i * 20, 1.0, r);
    }
    mf_history_flush_slice(64);
    n = mf_history_query_ts_step(UUID_B, "temp_f", 60, ts, v, 64);
    CHECK(n == 2, "two bins");
    CHECK(n == 2 && ts[0] < ts[1], "oldest first");
    CHECK(n == 2 && v[0] == 10.0 && v[1] == 40.0, "bin averages");
    n = mf_history_query_ts_step(UUID_B, "temp_f", 0, ts, v, 3);
    CHECK(n == 3 && v[2] == 50.0 && v[0] == 30.0, "raw newest 3, oldest first");
    CHECK(mf_history_query_ts_step(UUID_B, "conditions", 60, ts, v, 64) == -1,
          "text column refused");
    CHECK(mf_history_query_ts_step(UUID_B, "soc", 60, ts, v, 64) == -1,
          "undeclared column refused");
    CHECK(mf_history_query_ts_step(UUID_B, "temp_f;DROP TABLE samples", 60,
                                   ts, v, 64) == -1, "injection refused");
    CHECK(mf_history_query_ts_step(UUID_A, "soc", 60, ts, v, 64) == -1,
          "unknown module");
    mf_history_close();
    rmtree(dir);
}

/* Each module prunes its own file by its own policy. */
static void test_prune(void)
{
    char dir[64];
    double now = 1700000000.0, day = 86400.0;
    const char *keep1 = "{\"interval_s\":1,\"retention_days\":1,"
                        "\"columns\":{\"soc\":\"soc_pct\"}}";
    const char *nokeep = "{\"interval_s\":1,\"columns\":{\"soc\":\"soc_pct\"}}";
    sqlite3 *a, *b;
    int i;

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_A, "XD", "battery", "xd", keep1);
    mf_history_register(UUID_B, "Weather", "service", "weathergov", k_wx_spec);
    mf_history_register(UUID_C, "Old", "battery", "x", nokeep);
    CHECK(mf_history_retention(UUID_A) == 1.0, "default policy 1 day");
    CHECK(mf_history_retention(UUID_B) == 0.0, "weather keeps forever");
    CHECK(mf_history_retention(UUID_C) == -1.0, "no policy: not capturing");
    CHECK(mf_history_set_retention(UUID_C, 5) == -1, "no policy to set");
    for (i = 0; i < 5; i++)                  /* 5 old + 1 fresh each */
    {
        sample(UUID_A, now - (7 - i) * day, 0.5, "{\"soc_pct\":1}");
        sample(UUID_B, now - (7 - i) * day, 0.5, "{\"weather\":{\"temp_f\":1}}");
        sample(UUID_C, now - (7 - i) * day, 0.5, "{\"soc_pct\":1}");
    }
    sample(UUID_A, now - 0.5 * day, 0.5, "{\"soc_pct\":2}");
    sample(UUID_B, now - 0.5 * day, 0.5, "{\"weather\":{\"temp_f\":2}}");
    mf_history_flush_slice(64);
    a = mf_history_module_db(UUID_A);
    b = mf_history_module_db(UUID_B);
    CHECK(count_sql(mf_history_module_db(UUID_C), "SELECT COUNT(*) FROM samples")
          == 0, "spec without a policy captures nothing");

    /* Batches: a backlog goes a slice at a time and stays due. */
    CHECK(mf_history_prune_slice(now, 2) == 2, "first batch");
    CHECK(mf_history_prune_slice(now, 2) == 2, "still due");
    CHECK(mf_history_prune_slice(now, 2) == 1, "last of the backlog");
    CHECK(mf_history_prune_slice(now, 2) == 0, "not due again yet");
    CHECK(count_sql(a, "SELECT COUNT(*) FROM samples") == 1, "fresh row kept");
    CHECK(count_sql(b, "SELECT COUNT(*) FROM samples") == 6, "0 = forever");

    /* The user's policy overrides the default and is due at once. */
    CHECK(mf_history_set_retention(UUID_B, 2) == 0, "set weather 2 days");
    CHECK(mf_history_prune_slice(now, 500) == 5, "weather pruned");
    CHECK(count_sql(b, "SELECT COUNT(*) FROM samples") == 1, "weather fresh kept");
    CHECK(mf_history_retention(UUID_B) == 2.0, "policy in effect");
    mf_history_set_retention(UUID_B, -1);
    CHECK(mf_history_retention(UUID_B) == 0.0, "-1 restores the default");
    CHECK(mf_history_prune_slice(now + 2 * 3600, 500) == 0, "hourly recheck");
    mf_history_close();
    rmtree(dir);
}

/* ---- migration from the pre-0.5 shared file ------------------------- */

static void make_old_db(const char *path)
{
    sqlite3 *db = NULL;
    char *sql;
    int i;

    unlink(path);
    sqlite3_open(path, &db);
    sqlite3_exec(db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE devices (id TEXT PRIMARY KEY, name TEXT NOT NULL,"
        " kind TEXT NOT NULL, driver TEXT NOT NULL, created_ts REAL NOT NULL,"
        " retired_ts REAL);"
        "CREATE TABLE samples (id INTEGER PRIMARY KEY, ts REAL NOT NULL,"
        " device_id TEXT NOT NULL, online INTEGER NOT NULL, pack_v REAL,"
        " current_a REAL, power_w REAL, soc REAL, extra_json TEXT NOT NULL);"
        "INSERT INTO devices VALUES ('" UUID_A "','XD','battery','xd',1600000000,NULL);"
        "INSERT INTO devices VALUES ('" UUID_B "','Weather','service','weathergov',1600000000,NULL);"
        "INSERT INTO devices VALUES ('" UUID_C "','Old JK','battery','jk',1500000000,NULL);",
        NULL, NULL, NULL);
    for (i = 0; i < 5; i++)
    {
        /* Old battery rows: no power_w in the JSON, only in the fixed column. */
        sql = sqlite3_mprintf(
            "INSERT INTO samples (ts, device_id, online, pack_v, current_a,"
            " power_w, soc, extra_json) VALUES (%d, '%s', 1, 52.0, 2.0, 104.0,"
            " %d, '{\"pack_voltage_v\":52.0,\"current_a\":2.0,\"soc_pct\":%d}');"
            "INSERT INTO samples (ts, device_id, online, extra_json) VALUES"
            " (%d, '%s', 1, '{\"weather\":{\"temp_f\":%d,\"conditions\":\"Clear\"}}');"
            "INSERT INTO samples (ts, device_id, online, soc, extra_json) VALUES"
            " (%d, '%s', 0, 90, '{}');",
            1600000000 + i * 10, UUID_A, 70 + i, 70 + i,
            1600000000 + i * 600, UUID_B, 60 + i,
            1500000000 + i, UUID_C);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
    }
    sqlite3_close(db);
}

static const char *spec_for(const char *kind, const char *driver)
{
    (void)driver;
    return strcmp(kind, "battery") == 0 ? k_bat_spec : NULL;
}

static void test_migrate(void)
{
    char dir[64], old[128], moved[160], buf[64];
    sqlite3 *db = NULL;
    struct stat sb;
    int rows;

    tmpdir(dir, sizeof(dir));
    snprintf(old, sizeof(old), "%s/history.sqlite", dir);
    snprintf(moved, sizeof(moved), "%s.migrated", old);
    make_old_db(old);

    mf_history_open(dir);
    /* A and B are running modules; C was removed long ago. */
    mf_history_register(UUID_A, "XD", "battery", "xd", k_bat_spec);
    mf_history_register(UUID_B, "Weather", "service", "weathergov", k_wx_spec);
    rows = mf_history_migrate(old, spec_for);
    CHECK(rows == 15, "all rows migrated");
    CHECK(stat(old, &sb) != 0, "old file retired");
    CHECK(stat(moved, &sb) == 0, "renamed .migrated");

    db = mf_history_module_db(UUID_A);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 5, "A rows");
    CHECK(real_sql(db, "SELECT soc FROM samples ORDER BY ts DESC") == 74.0,
          "A soc from JSON");
    CHECK(real_sql(db, "SELECT power_w FROM samples ORDER BY ts DESC") == 104.0,
          "A power_w from the old fixed column");
    text_sql(db, "SELECT value FROM module WHERE key='created_ts'", buf, sizeof(buf));
    CHECK(atof(buf) == 1600000000.0, "created_ts kept");
    db = mf_history_module_db(UUID_B);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples") == 5, "B rows");
    CHECK(real_sql(db, "SELECT MAX(temp_f) FROM samples") == 64.0, "B temp_f");
    CHECK(mf_history_module_db(UUID_C) == NULL, "removed module not left open");
    {
        char p[320];
        sqlite3 *c = NULL;

        snprintf(p, sizeof(p), "%s/" UUID_C ".sqlite", dir);
        CHECK(stat(p, &sb) == 0, "removed module got a file");
        sqlite3_open(p, &c);
        CHECK(count_sql(c, "SELECT COUNT(*) FROM samples WHERE online = 0") == 5,
              "C rows");
        CHECK(real_sql(c, "SELECT soc FROM samples") == 90.0,
              "C soc from fixed column");
        text_sql(c, "SELECT value FROM module WHERE key='retired_ts'",
                 buf, sizeof(buf));
        CHECK(atof(buf) == 1500000004.0, "removed module retired at last sample");
        sqlite3_close(c);
    }

    /* Nothing left to do. */
    CHECK(mf_history_migrate(old, spec_for) == 0, "second run no-op");

    /* Interrupted-run resume: the old file reappears, but modules already
     * marked migrated are not copied twice. */
    make_old_db(old);
    rows = mf_history_migrate(old, spec_for);
    CHECK(rows == 0, "no duplicate copy");
    CHECK(count_sql(mf_history_module_db(UUID_A), "SELECT COUNT(*) FROM samples")
          == 5, "A still 5");
    mf_history_close();
    rmtree(dir);
}

static void sample_at(const char *uuid, double ts, int online, const char *reading)
{
    mf_sample_t s;

    memset(&s, 0, sizeof(s));
    snprintf(s.uuid, sizeof(s.uuid), "%s", uuid);
    s.ts = ts;
    s.online = online;
    s.capture_interval_s = 1.0;
    snprintf(s.reading, sizeof(s.reading), "%s", reading);
    mf_history_enqueue(&s);
}

/* The watermark of column `col` and kind (1 high, 0 low); NULL if absent. */
static const mf_watermark_t *mark_of(const mf_watermark_t *mk, int n,
                                     const char *col, int high, const char *when)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(mk[i].column, col) == 0 && mk[i].high == high &&
            strcmp(mk[i].when, when ? when : "") == 0)
            return &mk[i];
    return NULL;
}

static void test_watermarks(void)
{
    char dir[64];
    double now = (double)time(NULL);
    mf_watermark_t mk[MF_HISTORY_MAX_MARKS];
    const mf_watermark_t *hi, *lo, *on;
    int n;
    const char *spec =
        "{\"interval_s\":1,\"retention_days\":0,\"columns\":{"
        "\"power_w\":\"power_w\",\"soc\":\"soc_pct\",\"running\":\"running\","
        "\"note\":{\"path\":\"note\",\"type\":\"text\"}},"
        "\"watermarks\":["
        "{\"column\":\"power_w\",\"kind\":\"high\",\"window_h\":1},"
        "{\"column\":\"soc\",\"kind\":\"low\"},"
        "{\"column\":\"power_w\",\"kind\":\"high\",\"when\":\"running\"},"
        "{\"column\":\"nope\",\"kind\":\"high\"},"
        "{\"column\":\"soc\",\"kind\":\"middle\"},"
        "{\"column\":\"note\",\"kind\":\"high\"},"
        "{\"column\":\"soc\",\"kind\":\"low\",\"when\":\"note\"}]}";

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    CHECK(mf_history_register(UUID_A, "X", "charger", "x", spec) == 0, "register");
    n = mf_history_watermarks(UUID_A, mk, MF_HISTORY_MAX_MARKS);
    CHECK(n == 3, "three good watermarks; unknown column, kind, text kept out");
    CHECK(mf_history_watermarks(UUID_C, mk, MF_HISTORY_MAX_MARKS) == -1,
          "unknown module");
    hi = mark_of(mk, n, "power_w", 1, NULL);
    CHECK(hi && !hi->have && hi->window_s == 3600.0, "high: empty, 1 h window");
    lo = mark_of(mk, n, "soc", 0, NULL);
    CHECK(lo && lo->window_s == 86400.0, "low: 24 h by default");

    sample_at(UUID_A, now - 7200, 1, "{\"power_w\":9000,\"soc_pct\":90,\"running\":true}");
    sample_at(UUID_A, now - 600, 1, "{\"power_w\":500,\"soc_pct\":80,\"running\":true}");
    sample_at(UUID_A, now - 300, 1, "{\"power_w\":800,\"soc_pct\":60,\"running\":false}");
    sample_at(UUID_A, now - 200, 0, "{\"power_w\":99999,\"soc_pct\":1,\"running\":true}");
    sample_at(UUID_A, now - 60, 1, "{\"power_w\":700,\"soc_pct\":70,\"running\":true}");
    mf_history_flush_slice(16);

    /* As the samples are written. */
    n = mf_history_watermarks(UUID_A, mk, MF_HISTORY_MAX_MARKS);
    hi = mark_of(mk, n, "power_w", 1, NULL);
    lo = mark_of(mk, n, "soc", 0, NULL);
    on = mark_of(mk, n, "power_w", 1, "running");
    CHECK(hi && hi->have && hi->value == 800.0 && hi->ts == now - 300,
          "live high: 9000 left the hour, offline 99999 ignored");
    CHECK(lo && lo->have && lo->value == 60.0, "live low");
    CHECK(on && on->have && on->value == 9000.0, "live high while on");

    /* As read back from the file. */
    CHECK(mf_history_watermark_slice(now) == 1, "slice reads the module");
    CHECK(mf_history_watermark_slice(now) == 0, "then not again for a minute");
    n = mf_history_watermarks(UUID_A, mk, MF_HISTORY_MAX_MARKS);
    hi = mark_of(mk, n, "power_w", 1, NULL);
    lo = mark_of(mk, n, "soc", 0, NULL);
    on = mark_of(mk, n, "power_w", 1, "running");
    CHECK(hi && hi->have && hi->value == 800.0 && hi->ts == now - 300, "file high");
    CHECK(lo && lo->have && lo->value == 60.0 && lo->ts == now - 300, "file low");
    CHECK(on && on->have && on->value == 9000.0 && on->ts == now - 7200,
          "file high counts only samples with \"running\"");

    /* Two hours on, the 1 h window is empty. */
    CHECK(mf_history_watermark_slice(now + 7200) == 1, "slice later");
    n = mf_history_watermarks(UUID_A, mk, MF_HISTORY_MAX_MARKS);
    hi = mark_of(mk, n, "power_w", 1, NULL);
    lo = mark_of(mk, n, "soc", 0, NULL);
    CHECK(hi && !hi->have, "high gone once its window is empty");
    CHECK(lo && lo->have && lo->value == 60.0, "low still inside 24 h");
    mf_history_close();
    rmtree(dir);
}

/* A column added by a newer spec is filled in from the stored readings of
 * the last two days, so a watermark on it is whole at once. */
static void test_watermark_backfill(void)
{
    char dir[64];
    double now = (double)time(NULL);
    mf_watermark_t mk[MF_HISTORY_MAX_MARKS];
    sqlite3 *db;
    const char *v1 = "{\"interval_s\":1,\"retention_days\":0,\"columns\":{"
                     "\"dc_power_w\":\"dc_power_w\"}}";
    const char *v2 = "{\"interval_s\":1,\"retention_days\":0,\"columns\":{"
                     "\"dc_power_w\":\"dc_power_w\",\"inverting\":\"inverting\"},"
                     "\"watermarks\":[{\"column\":\"dc_power_w\",\"kind\":\"high\","
                     "\"when\":\"inverting\"}]}";

    tmpdir(dir, sizeof(dir));
    mf_history_open(dir);
    mf_history_register(UUID_A, "Master", "inverter", "magnum", v1);
    sample_at(UUID_A, now - 5 * 86400, 1, "{\"dc_power_w\":5000,\"inverting\":true}");
    sample_at(UUID_A, now - 3600, 1, "{\"dc_power_w\":2100,\"inverting\":false}");
    sample_at(UUID_A, now - 1800, 1, "{\"dc_power_w\":1200,\"inverting\":true}");
    mf_history_flush_slice(8);
    mf_history_close();

    mf_history_open(dir);
    CHECK(mf_history_register(UUID_A, "Master", "inverter", "magnum", v2) == 0,
          "re-register with the new column");
    db = mf_history_module_db(UUID_A);
    CHECK(count_sql(db, "SELECT COUNT(*) FROM samples WHERE inverting IS NOT NULL") == 2,
          "the last two days are backfilled, older rows are not");
    CHECK(mf_history_watermark_slice(now) == 1, "slice");
    CHECK(mf_history_watermarks(UUID_A, mk, MF_HISTORY_MAX_MARKS) == 1 &&
          mk[0].have && mk[0].value == 1200.0,
          "inverting-only high: grid charging (2100) left out");
    mf_history_close();
    rmtree(dir);
}

int main(void)
{
    test_columns();
    test_capture_gate();
    test_schema_evolution();
    test_retire();
    test_query_step();
    test_prune();
    test_migrate();
    test_watermarks();
    test_watermark_backfill();
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_history: ok\n");
    return 0;
}
