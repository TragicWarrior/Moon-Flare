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
    double poll_interval_s;
} mf_sample_t;

int  mf_history_open(const char *path);
void mf_history_close(void);
int  mf_history_upsert_device(const char *uuid, const char *name,
                              const char *kind, const char *driver,
                              double created_ts);
int  mf_history_retire_device(const char *uuid, double retired_ts);
int  mf_history_enqueue(const mf_sample_t *s);
int  mf_history_flush_slice(int max_rows);

/* Tests / debug only. NULL if closed. */
struct sqlite3 *mf_history_db(void);

#endif
