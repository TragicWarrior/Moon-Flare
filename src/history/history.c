#include "history.h"

#include <cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char name[32];
    char path[64];
    int  is_text;
} hist_col_t;

typedef struct {
    int           used;
    char          uuid[40];
    char          kind[16];
    sqlite3      *db;
    sqlite3_stmt *ins;              /* INSERT for the current column set */
    int           ncols;
    hist_col_t    cols[MF_HISTORY_MAX_COLS];
    char          graph[32];
    double        last_ts;          /* last queued sample, for downsampling */
    int           in_txn;           /* inside flush_slice's BEGIN */
    int           captures;         /* has a (valid) capture spec */
    double        keep_default;     /* spec "retention_days" */
    double        keep_days;        /* user's; < 0: keep_default; 0: forever */
    double        next_prune;       /* when this module next prunes itself */
} hist_mod_t;

static char        g_dir[256];
static int         g_open;
static hist_mod_t  g_mod[MF_HISTORY_MAX_MODULES];
static size_t      ring_head;
static size_t      ring_count;
static mf_sample_t ring[MF_HISTORY_RING_CAP];

/* ---- helpers ------------------------------------------------------- */

static hist_mod_t *find_mod(const char *uuid)
{
    int i;

    for (i = 0; uuid && i < MF_HISTORY_MAX_MODULES; i++)
        if (g_mod[i].used && strcmp(g_mod[i].uuid, uuid) == 0)
            return &g_mod[i];
    return NULL;
}

/* Column and uuid names are interpolated into SQL / file paths. */
static int safe_name(const char *s, size_t max)
{
    size_t i;

    if (!s || !s[0] || strlen(s) >= max)
        return 0;
    for (i = 0; s[i]; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) ||
              s[i] == '_'))
            return 0;
    return 1;
}

static int safe_uuid(const char *s)
{
    size_t i;

    if (!s || !s[0] || strlen(s) >= 40)
        return 0;
    for (i = 0; s[i]; i++)
        if (!(isxdigit((unsigned char)s[i]) || s[i] == '-'))
            return 0;
    return 1;
}

static int set_meta(sqlite3 *db, const char *key, const char *val)
{
    sqlite3_stmt *st = NULL;
    int rc;

    if (sqlite3_prepare_v2(db, "INSERT INTO module (key, value) VALUES (?, ?)"
                           " ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (val)
        sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 2);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int get_meta(sqlite3 *db, const char *key, char *out, size_t cap)
{
    sqlite3_stmt *st = NULL;
    int found = 0;

    out[0] = '\0';
    if (sqlite3_prepare_v2(db, "SELECT value FROM module WHERE key = ?", -1,
                           &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
    {
        snprintf(out, cap, "%s", (const char *)sqlite3_column_text(st, 0));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static sqlite3 *open_module_file(const char *uuid)
{
    char path[320];
    sqlite3 *db = NULL;
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS module ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS samples ("
        "  id INTEGER PRIMARY KEY,"
        "  ts REAL NOT NULL,"
        "  online INTEGER NOT NULL,"
        "  reading TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts);";

    snprintf(path, sizeof(path), "%s/%s.sqlite", g_dir, uuid);
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK)
    {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK)
    {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* Parse a capture spec into m->cols / m->graph / m->keep_default.  0 ok
 * (possibly no columns), -1 when the module does not capture: no spec, or
 * one without the required "retention_days" pruning policy. */
static int parse_spec(hist_mod_t *m, const char *spec)
{
    cJSON *s = spec && spec[0] ? cJSON_Parse(spec) : NULL;
    const cJSON *cols, *c, *g, *keep;

    m->ncols = 0;
    m->graph[0] = '\0';
    keep = cJSON_GetObjectItemCaseSensitive(s, "retention_days");
    if (!cJSON_IsObject(s) || !cJSON_IsNumber(keep) || keep->valuedouble < 0.0)
    {
        cJSON_Delete(s);
        return -1;
    }
    m->keep_default = keep->valuedouble;
    cols = cJSON_GetObjectItemCaseSensitive(s, "columns");
    cJSON_ArrayForEach(c, cols)
    {
        hist_col_t *hc;
        const char *path = NULL;
        int is_text = 0;

        if (m->ncols >= MF_HISTORY_MAX_COLS || !safe_name(c->string, 32) ||
            strcmp(c->string, "id") == 0 || strcmp(c->string, "ts") == 0 ||
            strcmp(c->string, "online") == 0 || strcmp(c->string, "reading") == 0)
            continue;
        if (cJSON_IsString(c))
            path = c->valuestring;
        else if (cJSON_IsObject(c))
        {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(c, "path");
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(c, "type");

            path = cJSON_IsString(p) ? p->valuestring : NULL;
            is_text = cJSON_IsString(t) && strcmp(t->valuestring, "text") == 0;
        }
        if (!path || !path[0] || strlen(path) >= sizeof(hc->path))
            continue;
        hc = &m->cols[m->ncols++];
        snprintf(hc->name, sizeof(hc->name), "%s", c->string);
        snprintf(hc->path, sizeof(hc->path), "%s", path);
        hc->is_text = is_text;
    }
    g = cJSON_GetObjectItemCaseSensitive(s, "graph");
    if (cJSON_IsString(g) && safe_name(g->valuestring, sizeof(m->graph)))
        snprintf(m->graph, sizeof(m->graph), "%s", g->valuestring);
    cJSON_Delete(s);
    return 0;
}

/* Add declared columns the file does not have yet; (re)build the INSERT. */
static int sync_columns(hist_mod_t *m)
{
    char sql[1024];
    size_t off;
    int i;

    for (i = 0; i < m->ncols; i++)
    {
        sqlite3_stmt *st = NULL;
        int have = 0;

        if (sqlite3_prepare_v2(m->db, "SELECT 1 FROM pragma_table_info('samples')"
                               " WHERE name = ?", -1, &st, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(st, 1, m->cols[i].name, -1, SQLITE_TRANSIENT);
        have = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (!have)
        {
            snprintf(sql, sizeof(sql), "ALTER TABLE samples ADD COLUMN %s %s",
                     m->cols[i].name, m->cols[i].is_text ? "TEXT" : "REAL");
            if (sqlite3_exec(m->db, sql, NULL, NULL, NULL) != SQLITE_OK)
                return -1;
        }
    }
    if (m->ins)
    {
        sqlite3_finalize(m->ins);
        m->ins = NULL;
    }
    off = (size_t)snprintf(sql, sizeof(sql),
                           "INSERT INTO samples (ts, online, reading");
    for (i = 0; i < m->ncols; i++)
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, ", %s",
                                m->cols[i].name);
    off += (size_t)snprintf(sql + off, sizeof(sql) - off, ") VALUES (?, ?, ?");
    for (i = 0; i < m->ncols; i++)
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, ", ?");
    snprintf(sql + off, sizeof(sql) - off, ")");
    return sqlite3_prepare_v2(m->db, sql, -1, &m->ins, NULL) == SQLITE_OK ? 0 : -1;
}

/* "weather.temp_f" in a parsed reading. */
static const cJSON *at_path(const cJSON *root, const char *path)
{
    char key[64];
    const char *p = path;
    const cJSON *cur = root;

    while (cur && *p)
    {
        size_t n = strcspn(p, ".");

        if (n >= sizeof(key))
            return NULL;
        memcpy(key, p, n);
        key[n] = '\0';
        cur = cJSON_GetObjectItemCaseSensitive(cur, key);
        p += n;
        if (*p == '.')
            p++;
    }
    return cur;
}

/* Insert one sample.  fallback (optional): old-table row values, used for a
 * same-named column the reading does not have (migration only). */
static int insert_sample(hist_mod_t *m, double ts, int online,
                         const char *reading, sqlite3_stmt *fallback)
{
    cJSON *r = cJSON_Parse(reading && reading[0] ? reading : "{}");
    int i, rc;

    sqlite3_reset(m->ins);
    sqlite3_clear_bindings(m->ins);
    sqlite3_bind_double(m->ins, 1, ts);
    sqlite3_bind_int(m->ins, 2, online);
    sqlite3_bind_text(m->ins, 3, reading && reading[0] ? reading : "{}", -1,
                      SQLITE_TRANSIENT);
    for (i = 0; i < m->ncols; i++)
    {
        const cJSON *v = at_path(r, m->cols[i].path);
        int slot = 4 + i;

        if (m->cols[i].is_text && cJSON_IsString(v))
            sqlite3_bind_text(m->ins, slot, v->valuestring, -1, SQLITE_TRANSIENT);
        else if (!m->cols[i].is_text && cJSON_IsNumber(v))
            sqlite3_bind_double(m->ins, slot, v->valuedouble);
        else if (!m->cols[i].is_text && cJSON_IsBool(v))
            sqlite3_bind_int(m->ins, slot, cJSON_IsTrue(v));
        else if (fallback)
        {
            int k, nc = sqlite3_column_count(fallback);

            for (k = 0; k < nc; k++)
                if (strcmp(sqlite3_column_name(fallback, k), m->cols[i].name) == 0 &&
                    sqlite3_column_type(fallback, k) != SQLITE_NULL)
                    sqlite3_bind_value(m->ins, slot, sqlite3_column_value(fallback, k));
        }
    }
    cJSON_Delete(r);
    rc = sqlite3_step(m->ins);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ---- lifecycle ----------------------------------------------------- */

int mf_history_open(const char *dir)
{
    if (!dir || !dir[0])
        return -1;
    mf_history_close();
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    if (mkdir(g_dir, 0755) < 0 && errno != EEXIST)
        return -1;
    g_open = 1;
    return 0;
}

static void close_mod(hist_mod_t *m)
{
    if (m->ins)
        sqlite3_finalize(m->ins);
    if (m->db)
        sqlite3_close(m->db);
    memset(m, 0, sizeof(*m));
}

void mf_history_close(void)
{
    int i;

    for (i = 0; i < MF_HISTORY_MAX_MODULES; i++)
        if (g_mod[i].used)
            close_mod(&g_mod[i]);
    ring_head = ring_count = 0;
    g_open = 0;
}

int mf_history_is_open(void)
{
    return g_open;
}

int mf_history_register(const char *uuid, const char *name, const char *kind,
                        const char *driver, const char *capture_json)
{
    hist_mod_t *m;
    char buf[32];
    int i;

    if (!g_open || !safe_uuid(uuid))
        return -1;
    m = find_mod(uuid);
    if (!m)
    {
        for (i = 0; i < MF_HISTORY_MAX_MODULES && g_mod[i].used; i++)
            ;
        if (i == MF_HISTORY_MAX_MODULES)
            return -1;
        m = &g_mod[i];
        memset(m, 0, sizeof(*m));
        m->db = open_module_file(uuid);
        if (!m->db)
            return -1;
        m->used = 1;
        m->last_ts = -1.0;
        m->keep_days = -1.0;
        snprintf(m->uuid, sizeof(m->uuid), "%s", uuid);
    }
    snprintf(m->kind, sizeof(m->kind), "%s", kind ? kind : "");
    m->captures = parse_spec(m, capture_json) == 0;
    if (sync_columns(m) != 0)
        return -1;
    set_meta(m->db, "uuid", uuid);
    set_meta(m->db, "name", name ? name : "");
    set_meta(m->db, "kind", kind ? kind : "");
    set_meta(m->db, "driver", driver ? driver : "");
    set_meta(m->db, "capture", capture_json && capture_json[0] ? capture_json : NULL);
    set_meta(m->db, "retired_ts", NULL);
    m->next_prune = 0.0;                /* policy may have changed: check */
    if (!get_meta(m->db, "created_ts", buf, sizeof(buf)))
    {
        snprintf(buf, sizeof(buf), "%.3f", (double)time(NULL));
        set_meta(m->db, "created_ts", buf);
    }
    return 0;
}

int mf_history_retire(const char *uuid, double retired_ts)
{
    hist_mod_t *m = find_mod(uuid);
    char buf[32];

    if (!m)
        return -1;
    mf_history_flush_slice(MF_HISTORY_RING_CAP);  /* its queued samples first */
    snprintf(buf, sizeof(buf), "%.3f", retired_ts);
    set_meta(m->db, "retired_ts", buf);
    close_mod(m);
    return 0;
}

/* ---- capture ------------------------------------------------------- */

int mf_history_enqueue(const mf_sample_t *s)
{
    hist_mod_t *m;
    size_t pos;

    if (!g_open || !s || s->capture_interval_s <= 0.0)
        return 0;
    m = find_mod(s->uuid);
    if (!m)
        return -1;
    if (!m->captures)
        return 0;
    if (m->last_ts >= 0.0 && s->ts - m->last_ts < s->capture_interval_s)
        return 0;                       /* downsample */
    m->last_ts = s->ts;
    if (ring_count == MF_HISTORY_RING_CAP)
    {
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
    int done = 0, i;

    if (!g_open || max_rows <= 0)
        return 0;
    while (done < max_rows && ring_count > 0)
    {
        mf_sample_t *s = &ring[ring_head];
        hist_mod_t *m = find_mod(s->uuid);

        if (m && m->ins)
        {
            if (!m->in_txn)
            {
                sqlite3_exec(m->db, "BEGIN", NULL, NULL, NULL);
                m->in_txn = 1;
            }
            if (insert_sample(m, s->ts, s->online, s->reading, NULL) != 0)
                fprintf(stderr, "history: %s: %s\n", m->uuid,
                        sqlite3_errmsg(m->db));
        }
        ring_head = (ring_head + 1) % MF_HISTORY_RING_CAP;
        ring_count--;
        done++;
    }
    for (i = 0; i < MF_HISTORY_MAX_MODULES; i++)
        if (g_mod[i].used && g_mod[i].in_txn)
        {
            sqlite3_exec(g_mod[i].db, "COMMIT", NULL, NULL, NULL);
            g_mod[i].in_txn = 0;
        }
    return done;
}

/* ---- pruning ------------------------------------------------------- */

static double keep_days_of(const hist_mod_t *m)
{
    return m->keep_days < 0.0 ? m->keep_default : m->keep_days;
}

int mf_history_set_retention(const char *uuid, double days)
{
    hist_mod_t *m = find_mod(uuid);
    char buf[32];

    if (!m || !m->captures)
        return -1;
    m->keep_days = days;
    m->next_prune = 0.0;
    snprintf(buf, sizeof(buf), "%g", keep_days_of(m));
    set_meta(m->db, "retention_days", buf);
    return 0;
}

double mf_history_retention(const char *uuid)
{
    hist_mod_t *m = find_mod(uuid);

    return m && m->captures ? keep_days_of(m) : -1.0;
}

/* One module applies its own policy to its own file, a batch at a time so
 * a large backlog never stalls the main loop. */
static int prune_module(hist_mod_t *m, double now, int max_rows)
{
    sqlite3_stmt *st = NULL;
    double days = keep_days_of(m);
    int n;

    if (days <= 0.0)                    /* 0: keep forever */
    {
        m->next_prune = now + MF_HISTORY_PRUNE_EVERY_S;
        return 0;
    }
    if (sqlite3_prepare_v2(m->db, "DELETE FROM samples WHERE id IN (SELECT id"
                           " FROM samples WHERE ts < ? ORDER BY ts LIMIT ?)",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_double(st, 1, now - days * 86400.0);
    sqlite3_bind_int(st, 2, max_rows);
    n = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(m->db) : 0;
    sqlite3_finalize(st);
    /* A full batch means more is due: come back on the next slice. */
    if (n < max_rows)
        m->next_prune = now + MF_HISTORY_PRUNE_EVERY_S;
    return n;
}

int mf_history_prune_slice(double now, int max_rows)
{
    int i, done = 0;

    if (!g_open || max_rows <= 0)
        return 0;
    for (i = 0; i < MF_HISTORY_MAX_MODULES && done < max_rows; i++)
    {
        hist_mod_t *m = &g_mod[i];

        if (!m->used || !m->captures || now < m->next_prune)
            continue;
        done += prune_module(m, now, max_rows - done);
    }
    return done;
}

/* ---- queries ------------------------------------------------------- */

const char *mf_history_graph_column(const char *uuid)
{
    hist_mod_t *m = find_mod(uuid);

    return m && m->graph[0] ? m->graph : NULL;
}

int mf_history_query_ts_step(const char *uuid, const char *column,
                             int step_s, double *ts, double *values, int max)
{
    hist_mod_t *m = find_mod(uuid);
    char sql[280];
    sqlite3_stmt *st = NULL;
    double *tb, *vb;
    int n = 0, i, known = 0;

    if (max <= 0)
        max = 800;
    if (max > 4096)
        max = 4096;
    if (!m || !column || !ts || !values)
        return -1;
    /* Only a declared numeric column: it is interpolated into the SQL. */
    for (i = 0; i < m->ncols; i++)
        if (strcmp(m->cols[i].name, column) == 0 && !m->cols[i].is_text)
            known = 1;
    if (!known)
        return -1;
    if (step_s > 0)
        snprintf(sql, sizeof(sql),
                 "SELECT (CAST(ts AS INTEGER) / ?1) * ?1, AVG(%s) FROM samples"
                 " WHERE %s IS NOT NULL GROUP BY 1 ORDER BY 1 DESC LIMIT ?2",
                 column, column);
    else
        snprintf(sql, sizeof(sql),
                 "SELECT ts, %s FROM samples WHERE %s IS NOT NULL"
                 " ORDER BY ts DESC LIMIT ?2", column, column);
    if (sqlite3_prepare_v2(m->db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (step_s > 0)
        sqlite3_bind_int(st, 1, step_s);
    sqlite3_bind_int(st, 2, max);
    tb = malloc((size_t)max * sizeof(double));
    vb = malloc((size_t)max * sizeof(double));
    if (!tb || !vb)
    {
        free(tb);
        free(vb);
        sqlite3_finalize(st);
        return -1;
    }
    while (n < max && sqlite3_step(st) == SQLITE_ROW)
    {
        tb[n] = sqlite3_column_double(st, 0);
        vb[n] = sqlite3_column_double(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    for (i = 0; i < n; i++)            /* newest-first -> oldest-first */
    {
        ts[i] = tb[n - 1 - i];
        values[i] = vb[n - 1 - i];
    }
    free(tb);
    free(vb);
    return n;
}

struct sqlite3 *mf_history_module_db(const char *uuid)
{
    hist_mod_t *m = find_mod(uuid);

    return m ? m->db : NULL;
}

/* ---- migration from the shared history.sqlite ---------------------- */

static int migrate_one(sqlite3 *old, const char *uuid, const char *name,
                       const char *kind, const char *driver, double created,
                       double retired, const char *spec)
{
    hist_mod_t *m;
    char flag[8], buf[32];
    sqlite3_stmt *st = NULL;
    double last_ts = 0.0;
    int rows = 0, was_registered = find_mod(uuid) != NULL;

    if (!was_registered &&
        mf_history_register(uuid, name, kind, driver, spec) != 0)
        return -1;
    m = find_mod(uuid);
    if (!m)
        return -1;
    if (get_meta(m->db, "migrated", flag, sizeof(flag)) && strcmp(flag, "1") == 0)
        return 0;                       /* done on an earlier run */
    if (!m->ins && sync_columns(m) != 0)
        return -1;
    if (sqlite3_prepare_v2(old, "SELECT ts, online, extra_json, pack_v,"
                           " current_a, power_w, soc FROM samples"
                           " WHERE device_id = ? ORDER BY ts", -1, &st,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_TRANSIENT);
    sqlite3_exec(m->db, "BEGIN", NULL, NULL, NULL);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *rj = (const char *)sqlite3_column_text(st, 2);

        if (insert_sample(m, sqlite3_column_double(st, 0),
                          sqlite3_column_int(st, 1), rj, st) != 0)
        {
            sqlite3_finalize(st);
            sqlite3_exec(m->db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        last_ts = sqlite3_column_double(st, 0);
        rows++;
    }
    sqlite3_finalize(st);
    snprintf(buf, sizeof(buf), "%.3f", created);
    set_meta(m->db, "created_ts", buf);  /* keep the original age */
    if (!was_registered)
    {
        /* No longer configured: retired when it was, else at its last sample. */
        snprintf(buf, sizeof(buf), "%.3f", retired > 0.0 ? retired : last_ts);
        set_meta(m->db, "retired_ts", buf);
    }
    set_meta(m->db, "migrated", "1");
    sqlite3_exec(m->db, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(m->db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
    if (!was_registered)
        close_mod(m);                   /* a removed module: file only */
    return rows;
}

int mf_history_migrate(const char *old_path,
                       const char *(*spec_for)(const char *kind,
                                               const char *driver))
{
    sqlite3 *old = NULL;
    sqlite3_stmt *st = NULL;
    char moved[320];
    struct stat sb;
    int total = 0, failed = 0;

    if (!g_open || !old_path || stat(old_path, &sb) != 0)
        return 0;
    if (sqlite3_open_v2(old_path, &old, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
    {
        sqlite3_close(old);
        return -1;
    }
    if (sqlite3_prepare_v2(old, "SELECT id, name, kind, driver, created_ts,"
                           " retired_ts FROM devices", -1, &st, NULL) != SQLITE_OK)
    {
        sqlite3_close(old);
        return -1;                      /* not the old layout */
    }
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *uuid = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *kind = (const char *)sqlite3_column_text(st, 2);
        const char *driver = (const char *)sqlite3_column_text(st, 3);
        hist_mod_t *live = find_mod(uuid);
        const char *spec = NULL;
        char specbuf[2048];
        int n;

        if (!live && spec_for)
            spec = spec_for(kind ? kind : "", driver ? driver : "");
        specbuf[0] = '\0';
        if (live)
            get_meta(live->db, "capture", specbuf, sizeof(specbuf));
        n = migrate_one(old, uuid, name, kind, driver,
                        sqlite3_column_double(st, 4),
                        sqlite3_column_double(st, 5),
                        live ? specbuf : spec);
        if (n < 0)
        {
            fprintf(stderr, "history: migrating %s failed\n", uuid ? uuid : "?");
            failed = 1;
        }
        else
            total += n;
    }
    sqlite3_finalize(st);
    /* Fold the WAL back in so one file carries everything, then retire it. */
    sqlite3_exec(old, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
    sqlite3_close(old);
    if (failed)
        return -1;                      /* keep it; the next start resumes */
    snprintf(moved, sizeof(moved), "%s.migrated", old_path);
    if (rename(old_path, moved) != 0)
        return -1;
    {
        char side[320];

        snprintf(side, sizeof(side), "%s-wal", old_path);
        unlink(side);
        snprintf(side, sizeof(side), "%s-shm", old_path);
        unlink(side);
    }
    return total;
}
