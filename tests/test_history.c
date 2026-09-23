#define _POSIX_C_SOURCE 200809L

#include "history.h"
#include <cJSON.h>

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static int tmpdb(char *path, size_t n)
{
    int fd;
    snprintf(path, n, "/tmp/mfhistXXXXXX");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static int select_count(const char *sql)
{
    sqlite3 *db = mf_history_db();
    sqlite3_stmt *st = NULL;
    int cnt = -1;
    if (!db || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        cnt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return cnt;
}

static void test_upsert_flush(void)
{
    char path[64];
    double now = (double)time(NULL);
    int i, flushed, cnt;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-001", "PackAlpha", "battery", "jk", now) == 0,
          "upsert");
    CHECK(mf_history_upsert_device("dev-001", "PackAlpha", "battery", "jk", now) == 0,
          "upsert again");
    for (i = 0; i < 100; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-001");
        s.ts = now + (double)i * 3.0;
        s.online = 1;
        s.has_pack_v = 1;
        s.pack_v = 52.0;
        s.has_current_a = 1;
        s.current_a = 10.0;
        s.has_power_w = 1;
        s.power_w = 520.0;
        s.has_soc = 1;
        s.soc = 50.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{\"i\":%d}", i);
        s.capture_interval_s = 2.0;
        CHECK(mf_history_enqueue(&s) == 0, "enqueue");
    }
    flushed = mf_history_flush_slice(1024);
    CHECK(flushed == 100, "flushed 100");
    cnt = select_count("SELECT COUNT(*) FROM samples");
    CHECK(cnt == 100, "COUNT samples = 100");
    cnt = select_count("SELECT COUNT(*) FROM devices WHERE id='dev-001'");
    CHECK(cnt == 1, "device row");
    mf_history_close();
    remove(path);
}

static void test_downsample(void)
{
    char path[64];
    double now = (double)time(NULL) + 1000.0;
    int i, flushed, cnt;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-down", "DownPack", "battery", "jk", now) == 0,
          "upsert");
    for (i = 0; i < 10; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-down");
        s.ts = now + (double)i * 0.1;
        s.online = 1;
        s.has_pack_v = 1;
        s.pack_v = 53.0;
        s.capture_interval_s = 2.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue downsample");
    }
    flushed = mf_history_flush_slice(1024);
    CHECK(flushed == 1, "downsample flush 1");
    cnt = select_count("SELECT COUNT(*) FROM samples");
    CHECK(cnt == 1, "downsample COUNT 1");
    mf_history_close();
    remove(path);
}

static void test_retire(void)
{
    char path[64];
    double now = (double)time(NULL) + 2000.0;
    int i, cnt;
    sqlite3_stmt *st = NULL;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-ret", "RetiredPack", "battery", "xd", now) == 0,
          "upsert");
    for (i = 0; i < 50; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-ret");
        s.ts = now + (double)i * 5.0;
        s.online = 1;
        s.has_pack_v = 1;
        s.pack_v = 54.0;
        s.capture_interval_s = 5.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue retire");
    }
    CHECK(mf_history_flush_slice(1024) == 50, "flush 50");
    CHECK(mf_history_retire_device("dev-ret", now + 500.0) == 0, "retire");
    cnt = select_count("SELECT COUNT(*) FROM samples");
    CHECK(cnt == 50, "samples remain after retire");
    CHECK(sqlite3_prepare_v2(mf_history_db(),
                             "SELECT COUNT(*), MIN(retired_ts) FROM devices WHERE id='dev-ret'",
                             -1, &st, NULL) == SQLITE_OK, "prepare retire");
    CHECK(sqlite3_step(st) == SQLITE_ROW, "retire row");
    CHECK(sqlite3_column_int(st, 0) == 1, "device still present");
    CHECK(sqlite3_column_double(st, 1) > 0.0, "retired_ts set");
    sqlite3_finalize(st);
    cnt = select_count(
        "SELECT COUNT(*) FROM samples s JOIN devices d ON s.device_id=d.id WHERE d.id='dev-ret'");
    CHECK(cnt == 50, "join still works");
    mf_history_close();
    remove(path);
}

static void test_flush_slices(void)
{
    char path[64];
    double now = (double)time(NULL) + 3000.0;
    int i, total = 0, cnt;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-flush", "FlushTest", "charger", "classic", now) == 0,
          "upsert");
    for (i = 0; i < 25; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-flush");
        s.ts = now + (double)i * 6.0;
        s.online = 1;
        s.has_pack_v = 1;
        s.pack_v = 48.0;
        s.capture_interval_s = 6.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue slice");
    }
    for (;;)
    {
        int n = mf_history_flush_slice(10);
        total += n;
        if (n == 0)
            break;
    }
    CHECK(total == 25, "sliced flush 25");
    cnt = select_count("SELECT COUNT(*) FROM samples");
    CHECK(cnt == 25, "COUNT 25");
    mf_history_close();
    remove(path);
}

static void test_power_w_query(void)
{
    char path[64];
    double now = (double)time(NULL) + 4000.0;
    double values[512];
    int i, n;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-pwr", "PwrTest", "charger", "classic", now) == 0,
          "upsert");
    for (i = 0; i < 10; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-pwr");
        s.ts = now + (double)i * 3.0;
        s.online = 1;
        s.has_pack_v = 1;
        s.pack_v = 52.0;
        s.has_power_w = 1;
        s.power_w = 100.0 + (double)i * 100.0;
        s.capture_interval_s = 3.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue power_w");
    }
    CHECK(mf_history_flush_slice(1024) == 10, "flush 10 power_w");
    n = mf_history_query("dev-pwr", "power_w", values, 512);
    CHECK(n == 10, "query returned 10");
    CHECK(n >= 3, "enough values for round-trip");
    if (n >= 3)
    {
        /* Values are newest first; last element is oldest. */
        CHECK(values[n - 1] == 100.0, "oldest power_w = 100");
        CHECK(values[n - 2] == 200.0, "mid power_w = 200");
        CHECK(values[n - 3] == 300.0, "newest of three = 300");
    }
    /* Bad column returns -1. */
    CHECK(mf_history_query("dev-pwr", "bogus", values, 10) < 0, "bad column returns -1");
    /* Non-existent device returns 0 rows (not -1). */
    CHECK(mf_history_query("no-such-dev", "power_w", values, 10) == 0,
          "missing dev returns 0 rows");
    mf_history_close();
    remove(path);
}

static void test_json_parse_history_fixture(void)
{
    /* Simulates the parsing path from ui_screen.c:
     * cJSON_Parse -> cJSON_GetObjectItemCaseSensitive("values") -> extract doubles.
     * This proves the JSON shape {"column":"power_w","values":[100,200,300]}
     * produces the correct double array without ncurses. */
    const char *fixture = "{\"column\":\"power_w\",\"values\":[100.0,200.0,300.0]}";
    cJSON *root, *arr;
    int i, n;
    double values[512];

    root = cJSON_Parse(fixture);
    CHECK(root != NULL, "parse fixture");
    arr = cJSON_GetObjectItemCaseSensitive(root, "values");
    CHECK(arr != NULL && cJSON_IsArray(arr), "values is array");
    n = cJSON_GetArraySize(arr);
    CHECK(n == 3, "array size 3");
    for (i = 0; i < n; i++)
    {
        cJSON *elem = cJSON_GetArrayItem(arr, i);
        values[i] = (elem && cJSON_IsNumber(elem)) ? elem->valuedouble : 0.0;
    }
    CHECK(values[0] == 100.0, "value[0]=100");
    CHECK(values[1] == 200.0, "value[1]=200");
    CHECK(values[2] == 300.0, "value[2]=300");
    cJSON_Delete(root);

    /* Edge: empty values array. */
    {
        const char *empty_fixture = "{\"column\":\"power_w\",\"values\":[]}";
        cJSON *r2, *a2;
        r2 = cJSON_Parse(empty_fixture);
        CHECK(r2 != NULL, "parse empty fixture");
        a2 = cJSON_GetObjectItemCaseSensitive(r2, "values");
        CHECK(a2 != NULL && cJSON_IsArray(a2), "empty values is array");
        CHECK(cJSON_GetArraySize(a2) == 0, "empty array size 0");
        cJSON_Delete(r2);
    }

    /* Edge: missing values field. */
    {
        const char *no_vals = "{\"column\":\"power_w\"}";
        cJSON *r3;
        r3 = cJSON_Parse(no_vals);
        CHECK(r3 != NULL, "parse no-values fixture");
        CHECK(cJSON_GetObjectItemCaseSensitive(r3, "values") == NULL,
              "missing values field");
        cJSON_Delete(r3);
    }
}

static void test_query_ts(void)
{
    char path[64];
    double now = (double)time(NULL) + 5000.0;
    double ts_arr[16], val_arr[16];
    int i, n;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-ts", "TsPack", "charger", "classic", now) == 0,
          "upsert");
    for (i = 0; i < 10; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-ts");
        s.ts = now + (double)i * 3.0;
        s.online = 1;
        s.has_power_w = 1;
        s.power_w = 100.0 + (double)i * 100.0;
        s.capture_interval_s = 3.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue ts");
    }
    CHECK(mf_history_flush_slice(1024) == 10, "flush 10 ts");
    n = mf_history_query_ts("dev-ts", "power_w", ts_arr, val_arr, 16);
    CHECK(n == 10, "query_ts returned 10");
    CHECK(n >= 3, "enough for checks");
    if (n >= 3)
    {
        /* Oldest first: first element = ts at now, value=100 */
        CHECK(ts_arr[0] == now, "oldest ts correct");
        CHECK(val_arr[0] == 100.0, "oldest value=100");
        /* Newest last: last element = ts at now+27, value=1000 */
        CHECK(ts_arr[n - 1] == now + 27.0, "newest ts correct");
        CHECK(val_arr[n - 1] == 1000.0, "newest value=1000");
        /* Timestamps ascending */
        for (i = 1; i < n; i++)
        {
            CHECK(ts_arr[i] > ts_arr[i - 1], "ts ascending");
        }
    }
    /* Bad column returns -1. */
    CHECK(mf_history_query_ts("dev-ts", "bogus", ts_arr, val_arr, 10) < 0,
          "bad column returns -1");
    /* Non-existent device returns 0. */
    CHECK(mf_history_query_ts("no-such-dev", "power_w", ts_arr, val_arr, 10) == 0,
          "missing dev returns 0 rows");
    mf_history_close();
    remove(path);
}

static void test_query_ts_step(void)
{
    char path[64];
    double now = (double)time(NULL) + 8000.0;
    double ts_arr[16], val_arr[16];
    int i, n;

    CHECK(tmpdb(path, sizeof(path)) == 0, "tmpdb");
    CHECK(mf_history_open(path) == 0, "open");
    CHECK(mf_history_upsert_device("dev-step", "StepPack", "charger", "classic", now) == 0,
          "upsert");
    for (i = 0; i < 6; i++)
    {
        mf_sample_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.uuid, sizeof(s.uuid), "dev-step");
        s.ts = now + (double)i * 90.0;
        s.online = 1;
        s.has_power_w = 1;
        s.power_w = 10.0 * (double)(i + 1);
        s.capture_interval_s = 10.0;
        snprintf(s.extra_json, sizeof(s.extra_json), "{}");
        CHECK(mf_history_enqueue(&s) == 0, "enqueue step");
    }
    CHECK(mf_history_flush_slice(1024) == 6, "flush 6 step");
    n = mf_history_query_ts_step("dev-step", "power_w", 60, ts_arr, val_arr, 16);
    CHECK(n == 6, "step query 6 minute bins");
    CHECK(val_arr[0] == 10.0, "oldest bin 10");
    CHECK(val_arr[n - 1] == 60.0, "newest bin 60");
    mf_history_close();
    remove(path);
}

int main(void)
{
    test_upsert_flush();
    test_downsample();
    test_retire();
    test_flush_slices();
    test_power_w_query();
    test_json_parse_history_fixture();
    test_query_ts();
    test_query_ts_step();
    mf_history_close();
    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("history: ok\n");
    return 0;
}
