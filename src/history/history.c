#include "history.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char   uuid[40];
    double last_ts;
} device_last_ts_t;

static sqlite3 *g_db;
static size_t   ring_head;
static size_t   ring_count;
static mf_sample_t ring[MF_HISTORY_RING_CAP];
static device_last_ts_t dev_last[128];
static int dev_last_count;

static int find_or_add_dev(const char *uuid)
{
    int i;
    for (i = 0; i < dev_last_count; i++) {
        if (strcmp(dev_last[i].uuid, uuid) == 0)
            return i;
    }
    if (dev_last_count >= 128)
        return -1;
    i = dev_last_count++;
    memset(&dev_last[i], 0, sizeof(dev_last[i]));
    snprintf(dev_last[i].uuid, sizeof(dev_last[i].uuid), "%s", uuid);
    dev_last[i].last_ts = -1.0;
    return i;
}

static int db_upsert_device(const char *uuid, const char *name,
                            const char *kind, const char *driver, double created_ts)
{
    static const char *sql =
        "INSERT INTO devices (id, name, kind, driver, created_ts, retired_ts)"
        " VALUES (?, ?, ?, ?, ?, NULL)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   name=excluded.name,"
        "   kind=excluded.kind,"
        "   driver=excluded.driver,"
        "   created_ts=excluded.created_ts,"
        "   retired_ts=NULL";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, driver, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 5, created_ts);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static int db_insert_sample(const mf_sample_t *s)
{
    static const char *sql =
        "INSERT INTO samples (ts, device_id, online, pack_v, current_a, power_w, soc, extra_json)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_double(st, 1, s->ts);
    sqlite3_bind_text(st, 2, s->uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, s->online);
    if (s->has_pack_v)
        sqlite3_bind_double(st, 4, s->pack_v);
    else
        sqlite3_bind_null(st, 4);
    if (s->has_current_a)
        sqlite3_bind_double(st, 5, s->current_a);
    else
        sqlite3_bind_null(st, 5);
    if (s->has_power_w)
        sqlite3_bind_double(st, 6, s->power_w);
    else
        sqlite3_bind_null(st, 6);
    if (s->has_soc)
        sqlite3_bind_double(st, 7, s->soc);
    else
        sqlite3_bind_null(st, 7);
    sqlite3_bind_text(st, 8, s->extra_json[0] ? s->extra_json : "{}", -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int mf_history_open(const char *path)
{
    int rc;
    char *err = NULL;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS devices ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  kind TEXT NOT NULL,"
        "  driver TEXT NOT NULL,"
        "  created_ts REAL NOT NULL,"
        "  retired_ts REAL"
        ");"
        "CREATE TABLE IF NOT EXISTS samples ("
        "  id INTEGER PRIMARY KEY,"
        "  ts REAL NOT NULL,"
        "  device_id TEXT NOT NULL,"
        "  online INTEGER NOT NULL,"
        "  pack_v REAL,"
        "  current_a REAL,"
        "  power_w REAL,"
        "  soc REAL,"
        "  extra_json TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_samples_dev_ts ON samples(device_id, ts);";

    if (g_db)
        mf_history_close();
    rc = sqlite3_open_v2(path, &g_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        g_db = NULL;
        return -1;
    }
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    rc = sqlite3_exec(g_db, schema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "history: schema: %s\n", err ? err : "");
        sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }
    ring_head = 0;
    ring_count = 0;
    dev_last_count = 0;
    memset(dev_last, 0, sizeof(dev_last));
    return 0;
}

void mf_history_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    ring_head = 0;
    ring_count = 0;
    dev_last_count = 0;
}

struct sqlite3 *mf_history_db(void)
{
    return g_db;
}

int mf_history_upsert_device(const char *uuid, const char *name,
                             const char *kind, const char *driver,
                             double created_ts)
{
    if (!g_db || !uuid || !name || !kind || !driver)
        return -1;
    return db_upsert_device(uuid, name, kind, driver, created_ts);
}

int mf_history_retire_device(const char *uuid, double retired_ts)
{
    sqlite3_stmt *st = NULL;
    int rc;

    if (!g_db || !uuid)
        return -1;
    rc = sqlite3_prepare_v2(g_db,
                            "UPDATE devices SET retired_ts = ? WHERE id = ?",
                            -1, &st, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_double(st, 1, retired_ts);
    sqlite3_bind_text(st, 2, uuid, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int mf_history_enqueue(const mf_sample_t *s)
{
    double interval;
    int didx;
    size_t pos;

    if (!g_db || !s)
        return -1;
    if (s->capture_interval_s <= 0.0)
        return 0; /* tracking off for this device */
    interval = s->capture_interval_s;
    didx = find_or_add_dev(s->uuid);
    if (didx >= 0 && dev_last[didx].last_ts >= 0.0) {
        if ((s->ts - dev_last[didx].last_ts) < interval)
            return 0; /* downsample */
    }
    if (didx >= 0)
        dev_last[didx].last_ts = s->ts;

    if (ring_count == MF_HISTORY_RING_CAP) {
        fprintf(stderr, "WARNING: history ring full, dropping oldest\n");
        ring_head = (ring_head + 1) % MF_HISTORY_RING_CAP;
        ring_count--;
    }
    pos = (ring_head + ring_count) % MF_HISTORY_RING_CAP;
    memcpy(&ring[pos], s, sizeof(*s));
    ring_count++;
    return 0;
}

int mf_history_flush_slice(int max_rows)
{
    int inserted = 0;
    int to_flush;
    int i;

    if (!g_db || max_rows <= 0 || ring_count == 0)
        return 0;
    to_flush = (int)ring_count;
    if (to_flush > max_rows)
        to_flush = max_rows;

    sqlite3_exec(g_db, "BEGIN", NULL, NULL, NULL);
    for (i = 0; i < to_flush; i++) {
        size_t pos = ring_head % MF_HISTORY_RING_CAP;
        if (db_insert_sample(&ring[pos]) != 0) {
            sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
            return inserted;
        }
        inserted++;
        ring_head = (ring_head + 1) % MF_HISTORY_RING_CAP;
        ring_count--;
    }
    sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);
    return inserted;
}
