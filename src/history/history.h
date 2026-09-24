#ifndef MF_HISTORY_H
#define MF_HISTORY_H

/* Captured history, one SQLite file per module:
 *
 *   <dir>/<module-uuid>.sqlite
 *     module (key, value)   uuid, name, kind, driver, created_ts,
 *                           retired_ts, capture (the spec), migrated
 *     samples (id, ts, online, reading, <declared columns>...)
 *
 * Whether a module captures, how often by default, and which reading fields
 * become columns is the module's own "capture" spec, advertised by its
 * plugin's describe():
 *
 *   {"interval_s": 600, "min_s": 60, "retention_days": 365, "graph": "temp_f",
 *    "columns": {"temp_f": "weather.temp_f",
 *                "conditions": {"path": "weather.conditions", "type": "text"}}}
 *
 * "retention_days" (required; a spec without it captures nothing) is the
 * module's default pruning policy: samples older than that many days are
 * deleted from its file.  0 keeps them forever.  The user can override it
 * per module; each module prunes only its own file.
 *
 * A column maps a name ([a-z0-9_], up to 31 chars) to a dotted path in the
 * reading; numeric unless "type" is "text".  The whole reading is always
 * kept in "reading" as JSON.  Columns a newer plugin declares are added to
 * an existing file. */

#include <stddef.h>

#define MF_HISTORY_RING_CAP    1024
#define MF_HISTORY_MAX_MODULES 32
#define MF_HISTORY_MAX_COLS    16
#define MF_HISTORY_PRUNE_EVERY_S 3600.0   /* a module re-checks its policy */

typedef struct {
    char   uuid[40];
    double ts;
    int    online;
    char   reading[4096];
    double capture_interval_s;   /* <= 0: do not capture */
} mf_sample_t;

/* Per-module files go in `dir` (created if missing). */
int  mf_history_open(const char *dir);
void mf_history_close(void);
int  mf_history_is_open(void);

/* Open or create the module's file and apply its capture spec (JSON text,
 * or NULL/"" when the module does not capture).  Safe to call again, e.g.
 * after a rename or a plugin upgrade. */
int  mf_history_register(const char *uuid, const char *name,
                         const char *kind, const char *driver,
                         const char *capture_json);
/* Module removed: note it and close its file (the file is kept). */
int  mf_history_retire(const char *uuid, double retired_ts);

/* Queue a sample; drops it inside the module's capture interval. */
int  mf_history_enqueue(const mf_sample_t *s);
/* Write up to max_rows queued samples; call from the main loop. */
int  mf_history_flush_slice(int max_rows);

/* The module's pruning policy in days (0 = forever); days < 0 restores the
 * plugin's default.  -1 when the module is unknown or does not capture. */
int    mf_history_set_retention(const char *uuid, double days);
/* The policy in effect, or -1 when the module does not capture. */
double mf_history_retention(const char *uuid);
/* Let each due module delete up to max_rows (in total) of its samples
 * older than its policy; call from the main loop.  Returns rows deleted. */
int    mf_history_prune_slice(double now, int max_rows);

/* The column the module's graph shows ("graph" in its spec), or NULL. */
const char *mf_history_graph_column(const char *uuid);

/* Up to max (ts, value) pairs of a declared numeric column, oldest first,
 * averaged into step_s-second bins (step_s <= 0: raw).  -1 when the module
 * or column is unknown. */
int  mf_history_query_ts_step(const char *uuid, const char *column,
                              int step_s, double *ts, double *values, int max);

/* One-time move of the old shared history.sqlite into per-module files.
 * spec_for(kind, driver) gives the capture spec for modules that are no
 * longer registered (removed); it may return NULL.  Each module is copied
 * in one transaction and marked, so an interrupted run resumes; when all
 * are done the old file is renamed to <old_path>.migrated.  Returns the
 * rows copied, 0 when there is nothing to do, -1 on error. */
int  mf_history_migrate(const char *old_path,
                        const char *(*spec_for)(const char *kind,
                                                const char *driver));

/* Tests: the open handle of a registered module's file, or NULL. */
struct sqlite3 *mf_history_module_db(const char *uuid);

#endif
