#ifndef MF_CONFIG_H
#define MF_CONFIG_H

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declare cJSON for the public API. */
typedef struct cJSON cJSON;

/* ---------- Daemon config (moonflared.json) ---------- */

#define MF_DEV_BUS_SIZE       32
#define MF_DEV_UUID_SIZE      40
#define MF_DEV_NAME_SIZE      64
#define MF_DEV_DRIVER_SIZE    32
#define MF_DEV_KIND_SIZE      32
#define MF_DEV_USB_PATH_SIZE  128
#define MF_DEV_USB_SERIAL_SIZE 64
#define MF_DEV_USB_BYID_SIZE  256
#define MF_DEV_BLE_ADDR_SIZE  32
#define MF_DEV_BLE_ADAPTER_SIZE 16
#define MF_DEV_BLE_PROTO_SIZE 32
#define MF_DEV_BLE_PASSWORD_SIZE 64
#define MF_DEV_MODBUS_IP_SIZE 64
#define MF_DEV_MODBUS_MAC_SIZE 32
#define MF_HISTORY_PATH_SIZE  256

typedef struct {
    char path[MF_HISTORY_PATH_SIZE];
    bool enabled;
} mf_config_history_t;

typedef struct {
    char path[MF_DEV_USB_PATH_SIZE];
    char serial_id[MF_DEV_USB_SERIAL_SIZE];
    char by_id[MF_DEV_USB_BYID_SIZE];
    bool auto_port;
    int baud;
    int addr;
} mf_config_usb_t;

typedef struct {
    char address[MF_DEV_BLE_ADDR_SIZE];
    char adapter[MF_DEV_BLE_ADAPTER_SIZE];
    char protocol[MF_DEV_BLE_PROTO_SIZE];
    char password[MF_DEV_BLE_PASSWORD_SIZE];
    bool write_response;
    int connect_timeout_s;
} mf_config_ble_t;

typedef struct {
    char ip[MF_DEV_MODBUS_IP_SIZE];
    int port;
    int unit_id;
    char mac[MF_DEV_MODBUS_MAC_SIZE];
    int unit_device_id;
    bool auto_net;
} mf_config_modbus_t;

typedef struct {
    char uuid[MF_DEV_UUID_SIZE];
    char name[MF_DEV_NAME_SIZE];
    char kind[MF_DEV_KIND_SIZE];
    char driver[MF_DEV_DRIVER_SIZE];
    bool enabled;
    bool active;                 /* counts toward system totals; default true */
    double poll_interval_s;
    double capture_interval_s;   /* history sample interval; 0=off, default 10 */
    char bus[MF_DEV_BUS_SIZE];
    mf_config_usb_t usb;
    mf_config_ble_t ble;
    mf_config_modbus_t modbus;
} mf_config_device_t;

#define MF_MAX_DEVICES 16

/* Full-scale values for the system meters (dashboard Input and Discharge). */
#define MF_SYSTEM_INPUT_MAX_W_DEFAULT     3500.0
#define MF_SYSTEM_DISCHARGE_MAX_W_DEFAULT 3000.0

typedef struct {
    double input_max_w;
    double discharge_max_w;
} mf_config_system_t;

typedef struct {
    char listen[64];
    char plugin_dir[256];
    char gatt_bin[256];
    mf_config_history_t history;
    mf_config_system_t system;
    mf_config_device_t devices[MF_MAX_DEVICES];
    int n_devices;
    uint64_t config_gen;
} mf_daemon_config_t;

/* ---------- TUI config (moonflare.json) ---------- */

#define MF_MAX_PROFILES        16
#define MF_PROFILE_NAME_SIZE   48
#define MF_PROFILE_HOST_SIZE   128

typedef struct {
    char name[MF_PROFILE_NAME_SIZE];
    char host[MF_PROFILE_HOST_SIZE];
    int  port;
} mf_conn_profile_t;

typedef struct {
    mf_conn_profile_t profiles[MF_MAX_PROFILES];
    int    n_profiles;
    char   default_profile[MF_PROFILE_NAME_SIZE];
    double refresh_interval_s;
} mf_tui_config_t;

/* ---------- Combined config ---------- */

typedef struct {
    mf_daemon_config_t daemon_cfg;
    mf_tui_config_t tui_cfg;
} mf_config_t;

/* ---------- API ---------- */

/* Built-in defaults. */
void mf_config_defaults(mf_daemon_config_t *cfg);
void mf_tui_config_defaults(mf_tui_config_t *cfg);

/* Load daemon config: search --config PATH > $HOME/.config/moonflare/
 * > /etc/moonflare/ > built-in defaults.
 * Returns 0 on success, -1 on error (printable via strerror). */
int mf_config_load(const char *config_path_override, mf_daemon_config_t *cfg);

/* Load TUI config with same search strategy (filename="moonflare.json"). */
int mf_tui_config_load(const char *config_path_override, mf_tui_config_t *cfg);

/* Serialize config to JSON string (caller must cJSON_free). */
char *mf_config_serialize(const mf_daemon_config_t *cfg);
char *mf_tui_config_serialize(const mf_tui_config_t *cfg);

/* One device object as JSON (plugin open() spec). Caller free. */
char *mf_config_device_serialize(const mf_config_device_t *d);

/* Fill *dev from a device JSON object (uuid required). 0 ok, -1 skip. */
int mf_config_device_from_json(mf_config_device_t *dev, const cJSON *obj);

/* Identity key for uniqueness: usb:path, ble:addr, tcp:ip:port, or "". */
void mf_config_device_endpoint(const mf_config_device_t *d, char *buf, size_t cap);

/* Parse from JSON (caller must cJSON_free the returned cJSON*).
 * Returns NULL on parse error. */
cJSON *mf_config_deserialize_json(const char *json);
cJSON *mf_tui_config_deserialize_json(const char *json);

/* Resolve a profile to "host:port" in out[cap].
 * name != NULL  -> that profile (by name);
 * name == NULL  -> default_profile, else first profile.
 * Returns 0 and fills out on success; -1 if not found / no profiles. */
int mf_tui_config_endpoint(const mf_tui_config_t *cfg, const char *name,
                           char *out, size_t cap);

/* Apply parsed JSON to a config struct (preserving defaults for missing keys). */
void mf_config_apply_json(mf_daemon_config_t *cfg, const cJSON *root);
void mf_tui_config_apply_json(mf_tui_config_t *cfg, const cJSON *root);

/* Atomic save: write to temp file then rename.
 * Returns 0 on success, -1 on error. */
int mf_config_save(const mf_daemon_config_t *cfg, const char *path);

/* config_redact: zero out passwords in-memory for safe serialization
 * (e.g., for GET /config response). */
void mf_config_redact(mf_daemon_config_t *cfg);

/* Overlay merge: patch devices in base_cfg using overlay_cfg entries
 * matched by UUID. Only specified fields in overlay are merged. */
void mf_config_overlay_merge(mf_daemon_config_t *base_cfg,
                             const mf_daemon_config_t *overlay_cfg);

/* Daemon-owned working config (the daemon's source of truth once it
 * exists).  Path: $MF_STATE_CONFIG, else $STATE_DIRECTORY/moonflared.json,
 * else /var/lib/moonflare/moonflared.json.  On first start the daemon seeds
 * it from moonflared.json (--config or the search path) plus the legacy
 * settings overlay; after that /etc is not read.  Every change the daemon
 * accepts (settings, add, remove, PUT config) saves the whole document.
 * load: 1 loaded, 0 no state file yet (cfg untouched), -1 unreadable. */
const char *mf_config_state_path(void);
int         mf_config_load_state(mf_daemon_config_t *cfg);
int         mf_config_save_state(const mf_daemon_config_t *cfg);

/* Legacy writable settings overlay (name, poll) from before the state
 * config.  Read only when seeding the state config.
 * Path: $MF_SETTINGS_OVERLAY, else $STATE_DIRECTORY/settings.json,
 * else /var/lib/moonflare/settings.json. */
const char *mf_config_overlay_path(void);
int         mf_config_save_overlay(const mf_daemon_config_t *cfg);
int         mf_config_load_overlay(mf_daemon_config_t *cfg);

/* ---------- Helpers ---------- */

/* Search path resolution: returns the first existing file path found.
 * Returns malloc'd string (caller free) or NULL if none found.
 * filename is e.g. "moonflared.json". */
char *mf_config_resolve_path(const char *override_path,
                              const char *filename);

#endif /* MF_CONFIG_H */
