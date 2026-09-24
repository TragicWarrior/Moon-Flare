#ifndef MF_PLUGIN_H
#define MF_PLUGIN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#define MF_PLUGIN_ABI 1

enum {
    MF_CAP_READ           = 1u << 0,
    MF_CAP_WRITE_SETTINGS = 1u << 1,
    MF_CAP_ACTION_SWITCH  = 1u << 2,  /* MOSFET / similar */
    MF_CAP_ACTION_REFRESH = 1u << 3,
    MF_CAP_PROBE          = 1u << 4,
    MF_CAP_AUTO_PORT      = 1u << 5,  /* USB unique-id relocate */
    MF_CAP_AUTO_NET       = 1u << 6,  /* Classic IP:port discover */
};

#define MF_IO_WANT_READ  1u
#define MF_IO_WANT_WRITE 2u

#define MF_OK               0
#define MF_ERR_INVAL       -1
#define MF_ERR_OFFLINE     -2
#define MF_ERR_UNSUPPORTED -3
#define MF_ERR_BUSY        -4

typedef enum {
    MF_STEP_IDLE    = 0,
    MF_STEP_UPDATED = 1,
    MF_STEP_ERROR   = 2,
} mf_step_t;

typedef struct mf_plugin_ops {
    uint32_t    abi;
    uint32_t    ops_size;
    const char *kind;       /* "battery", "charger", … */
    const char *driver;     /* "xd", "jk", "classic", "demo" */
    const char *version;

    void *(*open)(const char *spec_json, char *err, size_t errsz);
    void  (*close)(void *ctx);

    int      (*fd)(void *ctx);
    unsigned (*select_mask)(void *ctx);
    void     (*prepare_fds)(void *ctx, fd_set *r, fd_set *w, int *maxfd); /* optional */

    mf_step_t (*step)(void *ctx);

    unsigned    (*caps)(void *ctx);
    const char *(*last_error)(void *ctx);

    /* Fill ONLY the kind-specific `data` object. Daemon wraps envelope. */
    int  (*get_reading)(void *ctx, char *json, size_t cap);

    int  (*get_settings)(void *ctx, char *json, size_t cap);
    int  (*put_settings)(void *ctx, const char *json, char *err, size_t errsz);

    int  (*action)(void *ctx, const char *action, const char *json,
                   char *err, size_t errsz);

    int        (*probe_start)(const char *args_json, void **job, char *err, size_t errsz);
    mf_step_t  (*probe_step)(void *job);
    unsigned   (*probe_select_mask)(void *job);
    void       (*probe_prepare_fds)(void *job, fd_set *r, fd_set *w, int *maxfd);
    int        (*probe_result)(void *job, char *json, size_t cap);
    void       (*probe_close)(void *job);

    /* ---- Optional, added in 0.3.0.  Plugins built before it end above;
     * the loader accepts them and treats these as NULL. ---- */

    /* Static JSON describing what the Add Module form asks for, so clients
     * need no knowledge of the plugin:
     *   {"bus": "usb-serial",
     *    "fields": [{"key": "usb.path", "label": "USB Path",
     *                "hint": "(device)", "type": "string",
     *                "default": "/dev/ttyUSB0", "required": true}, ...]}
     * key: dotted config path.  type: "string" | "number" | "bool".
     * default, hint, required: optional.  NULL: name + poll interval only. */
    const char *(*describe)(void);
} mf_plugin_ops_t;

/* Smallest ops_size the loader accepts: the table as it was before the
 * optional fields.  Grows only if a required field is ever added. */
#define MF_PLUGIN_OPS_MIN_SIZE offsetof(mf_plugin_ops_t, describe)

/*
 * *out = pointer to the first of N consecutive mf_plugin_ops_t.
 * Return N. Single-kind plugins return 1; libmf_demo.so returns 2.
 * KD 26 also described (out, n); the return value is n.
 */
size_t mf_plugin_entries(const mf_plugin_ops_t **out);

#endif
