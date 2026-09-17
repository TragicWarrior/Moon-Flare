#define _POSIX_C_SOURCE 200809L

#include "history.h"

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
    for (i = 0; i < 100; i++) {
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
    for (i = 0; i < 10; i++) {
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
    for (i = 0; i < 50; i++) {
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
    for (i = 0; i < 25; i++) {
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
    for (;;) {
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

int main(void)
{
    test_upsert_flush();
    test_downsample();
    test_retire();
    test_flush_slices();
    mf_history_close();
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("history: ok\n");
    return 0;
}
