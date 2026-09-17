#ifndef MF_HISTORY_H
#define MF_HISTORY_H

#include <stddef.h>

#define MF_HISTORY_RING_CAP 1024

typedef struct {
    char   uuid[40];
    double ts;
    int    online;
    double pack_v, current_a, power_w, soc;
    int    has_pack_v, has_current_a, has_power_w, has_soc;
    char   extra_json[4096];
    double capture_interval_s;
} mf_sample_t;

int  mf_history_open(const char *path);
void mf_history_close(void);
int  mf_history_upsert_device(const char *uuid, const char *name,
                              const char *kind, const char *driver,
                              double created_ts);
int  mf_history_retire_device(const char *uuid, double retired_ts);
int  mf_history_enqueue(const mf_sample_t *s);
int  mf_history_flush_slice(int max_rows);

/* Read up to max recent non-null values of one common column (pack_v,
    current_a, power_w or soc) for a device, newest first.  Returns the count
    written to out, or -1 on error (bad column / db not open). */
int  mf_history_query(const char *uuid, const char *column,
                       double *out, int max);

/* Read up to max (ts, value) pairs for a device, oldest-first.
    Queries newest-first (ORDER BY ts DESC LIMIT max) then reverses
    so the array is oldest→newest for the TUI (left→right buckets).
    ts and values arrays must each have room for at least max entries.
    Returns the count written, or -1 on error (bad column / db not open).
    Cap: 800 rows default when max <= 0 to keep JSON < ~18 KB. */
int  mf_history_query_ts(const char *uuid, const char *column,
                          double *ts, double *values, int max);

/* Like query_ts, but AVG into step_s-second bins (e.g. 60). step_s <= 0
   is raw samples. Oldest-first. */
int  mf_history_query_ts_step(const char *uuid, const char *column,
                              int step_s, double *ts, double *values, int max);

/* Tests / debug only. NULL if closed. */
struct sqlite3 *mf_history_db(void);

#endif
