#ifndef MF_DEVICE_H
#define MF_DEVICE_H

#include "config.h"
#include "loader.h"
#include "protothread.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#define MF_DEVICE_SLOTS    32
#define MF_UUID_LEN        37
#define MF_READING_JSON_SZ 16384

typedef struct mf_devinfo {
    const char *uuid;
    const char *name;
    const char *kind;
    const char *driver;
    const char *endpoint;
    bool        online;
    bool        dying;          /* in_use && stop */
    uint64_t    seq;
    unsigned    caps;
    const char *reading_json;   /* kind-specific data object, or "{}" */
    const char *last_error;
} mf_devinfo_t;

void mf_devices_init(protothread_t pts, char *chan_tick,
                     volatile sig_atomic_t *quit,
                     const mf_plugin_registry_t *reg);

/* Live slots only (in_use && !stop). */
int  mf_devices_visit_live(int (*fn)(const mf_devinfo_t *, void *), void *arg);
int  mf_devices_find_live(const char *uuid, mf_devinfo_t *out);

/* 0 ok; -1 bad; -2 unknown kind/driver; -3 name in use; -4 full; -5 open fail. */
int  mf_devices_add(const char *name, const char *kind, const char *driver,
                    const char *spec_json, const char *uuid_in,
                    char *uuid_out, size_t uuid_cap,
                    char *err, size_t errsz);

/* KD 29: diff live slots vs config devices[]. 0 applied, 1 pending (dying
 * stop+reopen), -1 validate error (err set). Call apply_pending on each tick. */
int  mf_devices_validate_config(const mf_config_device_t *devs, int n,
                                char *err, size_t errsz);
int  mf_devices_apply_config(const mf_config_device_t *devs, int n,
                             char *err, size_t errsz);
void mf_devices_apply_pending(void);

/* 202 if slot was live or dying; 404 if unknown. */
int  mf_devices_delete(const char *uuid);

/* Settings / actions (PR-10). HTTP status: 200, 400, 404, 409. */
int    mf_devices_get_settings(const char *uuid, char *json, size_t cap);
int    mf_devices_put_settings(const char *uuid, const char *json,
                               char *err, size_t errsz);
int    mf_devices_action(const char *uuid, const char *action, const char *json,
                         char *err, size_t errsz);
double mf_poll_interval_min(const char *driver);
int    mf_devices_any_dying(void);

void mf_devices_prepare_fds(fd_set *rset, fd_set *wset, int *maxfd);
void mf_devices_request_stop_all(void);

const mf_plugin_registry_t *mf_devices_registry(void);

#endif
