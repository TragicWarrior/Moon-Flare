/*
 * mf_gatt -- BlueZ helper, one process per adapter.
 * Abstract socket @mf-gatt/<adapter>. JSON-lines, mux by MAC.
 * Scan-before-connect serialized; dual FFE1 by Flags. No JK framing.
 */

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <systemd/sd-bus.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLI     8
#define LINE_CAP    8192
#define HEX_CAP     1024
#define NOTIFY_MAX  512
#define CONNECT_S   20.0
#define GATT_WAIT_S 4.0
#define SCAN_S      8.0
#define RECONNECT_S 1.0

#define BLUEZ      "org.bluez"
#define ADAPTER_IF "org.bluez.Adapter1"
#define DEVICE_IF  "org.bluez.Device1"
#define CHAR_IF    "org.bluez.GattCharacteristic1"
#define PROPS_IF   "org.freedesktop.DBus.Properties"
#define OM_IF      "org.freedesktop.DBus.ObjectManager"
#define FFE1       "0000ffe1-0000-1000-8000-00805f9b34fb"

enum {
    SESS_IDLE = 0,
    SESS_SCAN,
    SESS_CONNECT,
    SESS_WAIT_GATT,
    SESS_READY
};

typedef struct cli cli_t;

struct cli {
    int      fd;
    char     mac[32];
    int      bound;
    int      acked;
    char     in[LINE_CAP];
    size_t   in_len;
    char     out[LINE_CAP];
    size_t   out_len, out_off;

    int      sess;
    double   deadline;
    char     dev_path[128];
    char     write_path[192];
    char     notify_path[192];
    int      write_handle;
    int      notify_handle;
    int      write_response;
    sd_bus_slot *notify_slot;
    sd_bus_slot *dev_slot;
    double   reconnect_at;
};

static void sess_fail(cli_t *c, const char *msg);
static void begin_connect(cli_t *c, int idx);

static cli_t g_cli[MAX_CLI];
static const char *g_adapter = "hci0";
static char g_adapter_path[64];
static sd_bus *g_bus;
static int g_scan_owner = -1;
static int g_debug;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void log_msg(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "mf_gatt: %s\n", buf);
}

static int json_str(const char *js, const char *key, char *dst, size_t cap)
{
    char pat[64];
    const char *p, *q;
    size_t n;
    dst[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return -1;
    p++;
    q = strchr(p, '"');
    if (!q)
        return -1;
    n = (size_t)(q - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
    return 0;
}

static int json_bool(const char *js, const char *key, int def)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!strncmp(p, "true", 4))
        return 1;
    if (!strncmp(p, "false", 5))
        return 0;
    return def;
}

static double json_double(const char *js, const char *key, double def)
{
    const char *p;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    return atof(p + 1);
}

static int mac_eq(const char *a, const char *b)
{
    return a && b && strcasecmp(a, b) == 0;
}

static void mac_to_path(const char *mac, char *out, size_t cap)
{
    char tmp[32];
    size_t i, j = 0;
    snprintf(tmp, sizeof(tmp), "%s", mac);
    for (i = 0; tmp[i]; i++)
    {
        if (tmp[i] == ':')
            tmp[i] = '_';
        else if (tmp[i] >= 'a' && tmp[i] <= 'f')
            tmp[i] = (char)(tmp[i] - 'a' + 'A');
    }
    snprintf(out, cap, "%s/dev_%s", g_adapter_path, tmp);
    (void)j;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (in && in[0] && in[1] && n < cap)
    {
        int hi = hex_nibble((unsigned char)in[0]);
        int lo = hex_nibble((unsigned char)in[1]);
        if (hi < 0 || lo < 0)
            break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        in += 2;
    }
    return (int)n;
}

static void hex_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char *d = "0123456789abcdef";
    size_t i;
    if (cap < n * 2 + 1)
        n = (cap - 1) / 2;
    for (i = 0; i < n; i++)
    {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static void queue_line(cli_t *c, const char *s)
{
    size_t n = strlen(s);
    if (c->out_len + n + 1 >= sizeof(c->out))
        return;
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
    c->out[c->out_len++] = '\n';
}

static void queue_ok(cli_t *c, const char *cmd)
{
    char line[192];
    snprintf(line, sizeof(line),
             "{\"type\":\"ok\",\"cmd\":\"%s\",\"address\":\"%s\"}",
             cmd, c->mac);
    queue_line(c, line);
}

static void queue_err(cli_t *c, const char *msg)
{
    char line[256];
    snprintf(line, sizeof(line),
             "{\"type\":\"error\",\"address\":\"%s\",\"message\":\"%s\"}",
             c->mac[0] ? c->mac : "", msg);
    queue_line(c, line);
}

static void queue_state(cli_t *c, const char *st, const char *detail)
{
    char line[256];
    snprintf(line, sizeof(line),
             "{\"type\":\"state\",\"address\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}",
             c->mac, st, detail ? detail : "");
    queue_line(c, line);
}

static int mac_taken(const char *mac, int self)
{
    int i;
    for (i = 0; i < MAX_CLI; i++)
    {
        if (i == self || g_cli[i].fd < 0 || !g_cli[i].bound)
            continue;
        if (mac_eq(g_cli[i].mac, mac))
            return 1;
    }
    return 0;
}

static void sess_reset(cli_t *c)
{
    if (c->notify_slot)
    {
        sd_bus_slot_unref(c->notify_slot);
        c->notify_slot = NULL;
    }
    if (c->dev_slot)
    {
        sd_bus_slot_unref(c->dev_slot);
        c->dev_slot = NULL;
    }
    c->sess = SESS_IDLE;
    c->dev_path[0] = c->write_path[0] = c->notify_path[0] = '\0';
    c->write_handle = c->notify_handle = 0;
}

static int is_link_lost(const char *m)
{
    if (!m || !m[0])
        return 0;
    if (strcasestr(m, "not connected"))
        return 1;
    if (strstr(m, "doesn't exist") || strstr(m, "does not exist"))
        return 1;
    if (strcasestr(m, "unknown object"))
        return 1;
    if (strcasestr(m, "no such"))
        return 1;
    return 0;
}

static int char_handle_from_path(const char *path)
{
    const char *p = strrchr(path, '/');
    unsigned h = 0;
    if (!p)
        return 0;
    p++;
    if (strncmp(p, "char", 4) != 0)
        return 0;
    if (sscanf(p + 4, "%x", &h) != 1)
        return 0;
    return (int)h;
}

static int skip_variant(sd_bus_message *m)
{
    char t;
    const char *contents;
    int r = sd_bus_message_peek_type(m, &t, &contents);
    if (r <= 0)
        return r;
    if (t != SD_BUS_TYPE_VARIANT)
        return sd_bus_message_skip(m, NULL);
    r = sd_bus_message_enter_container(m, 'v', contents);
    if (r < 0)
        return r;
    r = sd_bus_message_skip(m, contents);
    (void)sd_bus_message_exit_container(m);
    return r;
}

/* Walk GetManagedObjects for this device; pick FFE1 write+notify by Flags. */
static int discover_ffe1(cli_t *c)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *rep = NULL;
    int r;
    char write_path[192] = "", notify_path[192] = "";
    int write_h = 0, notify_h = 0;
    int write_rank = 0; /* 1 combined, 2 write-only (preferred) */

    c->write_path[0] = c->notify_path[0] = '\0';
    r = sd_bus_call_method(g_bus, BLUEZ, "/", OM_IF, "GetManagedObjects",
                           &err, &rep, NULL);
    if (r < 0)
    {
        log_msg("GetManagedObjects: %s", err.message ? err.message : "fail");
        sd_bus_error_free(&err);
        return -1;
    }
    r = sd_bus_message_enter_container(rep, 'a', "{oa{sa{sv}}}");
    while (r > 0 && sd_bus_message_enter_container(rep, 'e', "oa{sa{sv}}") > 0)
    {
        const char *opath = NULL;
        sd_bus_message_read(rep, "o", &opath);
        if (!opath || strncmp(opath, c->dev_path, strlen(c->dev_path)) != 0)
        {
            sd_bus_message_skip(rep, "a{sa{sv}}");
            sd_bus_message_exit_container(rep);
            continue;
        }
        if (sd_bus_message_enter_container(rep, 'a', "{sa{sv}}") > 0)
        {
            while (sd_bus_message_enter_container(rep, 'e', "sa{sv}") > 0)
            {
                const char *iface = NULL;
                sd_bus_message_read(rep, "s", &iface);
                if (iface && strcmp(iface, CHAR_IF) == 0)
                {
                    char uuid[64] = "";
                    int has_notify = 0, has_write = 0, has_wnr = 0;
                    if (sd_bus_message_enter_container(rep, 'a', "{sv}") > 0)
                    {
                        while (sd_bus_message_enter_container(rep, 'e', "sv") > 0)
                        {
                            const char *key = NULL;
                            const char *contents = NULL;
                            sd_bus_message_read(rep, "s", &key);
                            sd_bus_message_peek_type(rep, NULL, &contents);
                            sd_bus_message_enter_container(rep, 'v', contents);
                            if (key && strcmp(key, "UUID") == 0 && contents &&
                                contents[0] == 's')
                            {
                                const char *u = NULL;
                                sd_bus_message_read(rep, "s", &u);
                                if (u)
                                    snprintf(uuid, sizeof(uuid), "%s", u);
                            } else if (key && strcmp(key, "Flags") == 0 &&
                                       contents && strcmp(contents, "as") == 0)
                            {
                                sd_bus_message_enter_container(rep, 'a', "s");
                                for (;;)
                                {
                                    const char *f = NULL;
                                    if (sd_bus_message_read(rep, "s", &f) <= 0)
                                        break;
                                    if (!strcasecmp(f, "notify") ||
                                        !strcasecmp(f, "indicate"))
                                        has_notify = 1;
                                    if (!strcasecmp(f, "write"))
                                        has_write = 1;
                                    if (!strcasecmp(f, "write-without-response"))
                                        has_wnr = 1;
                                }
                                sd_bus_message_exit_container(rep);
                            } else
                            {
                                sd_bus_message_skip(rep, contents);
                            }
                            sd_bus_message_exit_container(rep);
                            sd_bus_message_exit_container(rep);
                        }
                        sd_bus_message_exit_container(rep);
                    }
                    if (uuid[0] && strcasecmp(uuid, FFE1) == 0)
                    {
                        int h = char_handle_from_path(opath);
                        int rank;
                        /* Combined FFE1 (notify+write on one char): if Flags
                         * did not parse, still use this characteristic. */
                        if (!has_notify && !has_write && !has_wnr)
                            has_notify = has_write = 1;
                        log_msg("ffe1 %s h=%d n=%d w=%d wnr=%d",
                                opath, h, has_notify, has_write, has_wnr);
                        if (has_notify)
                        {
                            snprintf(notify_path, sizeof(notify_path), "%s", opath);
                            notify_h = h;
                        }
                        /* Prefer a write-only FFE1 (handle 0x03) over notify. */
                        rank = 0;
                        if (has_write || has_wnr)
                            rank = has_notify ? 1 : 2;
                        if (rank > write_rank)
                        {
                            snprintf(write_path, sizeof(write_path), "%s", opath);
                            write_h = h;
                            write_rank = rank;
                        }
                    }
                } else
                {
                    sd_bus_message_skip(rep, "a{sv}");
                }
                sd_bus_message_exit_container(rep);
            }
            sd_bus_message_exit_container(rep);
        }
        sd_bus_message_exit_container(rep);
    }
    sd_bus_message_exit_container(rep);
    sd_bus_message_unref(rep);

    /* Dual-FFE1: write handle 0x03 and notify missing → look for 0x05. */
    if (write_path[0] && write_h == 0x03 && !notify_path[0])
    {
        /* re-scan is heavy; path sibling char0005 */
        char guess[192];
        snprintf(guess, sizeof(guess), "%s", write_path);
        {
            char *p = strrchr(guess, '/');
            if (p)
                snprintf(p + 1, sizeof(guess) - (size_t)(p + 1 - guess),
                         "char0005");
        }
        snprintf(notify_path, sizeof(notify_path), "%s", guess);
        notify_h = 0x05;
    }
    if (!write_path[0] || !notify_path[0])
    {
        log_msg("ffe1 missing write=%d notify=%d for %s",
                write_path[0] ? 1 : 0, notify_path[0] ? 1 : 0, c->mac);
        return -1;
    }
    log_msg("ffe1 write=%s notify=%s", write_path, notify_path);
    snprintf(c->write_path, sizeof(c->write_path), "%s", write_path);
    snprintf(c->notify_path, sizeof(c->notify_path), "%s", notify_path);
    c->write_handle = write_h;
    c->notify_handle = notify_h;
    (void)skip_variant;
    return 0;
}

static int on_notify(sd_bus_message *m, void *userdata, sd_bus_error *ret_err)
{
    cli_t *c = userdata;
    const char *iface = NULL;
    static unsigned nlog;
    (void)ret_err;
    if (!c || c->fd < 0)
        return 0;
    if (sd_bus_message_read(m, "s", &iface) < 0)
        return 0;
    if (!iface || strcmp(iface, CHAR_IF) != 0)
        return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
        return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char *key = NULL;
        const char *contents = NULL;
        sd_bus_message_read(m, "s", &key);
        sd_bus_message_peek_type(m, NULL, &contents);
        if (key && strcmp(key, "Value") == 0 && contents &&
            strcmp(contents, "ay") == 0)
        {
            const uint8_t *bytes = NULL;
            size_t n = 0;
            if (sd_bus_message_enter_container(m, 'v', "ay") >= 0)
            {
                (void)sd_bus_message_read_array(m, 'y',
                                                (const void **)&bytes, &n);
                sd_bus_message_exit_container(m);
            }
            if (bytes && n > 0 && n <= NOTIFY_MAX)
            {
                char hex[HEX_CAP];
                char line[LINE_CAP];
                hex_encode(bytes, n, hex, sizeof(hex));
                snprintf(line, sizeof(line),
                         "{\"type\":\"notify\",\"address\":\"%s\",\"hex\":\"%s\"}",
                         c->mac, hex);
                queue_line(c, line);
                if (nlog < 8 || g_debug)
                {
                    log_msg("notify %s n=%zu", c->mac, n);
                    nlog++;
                }
            } else if (n > NOTIFY_MAX)
            {
                log_msg("notify %s dropped n=%zu", c->mac, n);
            }
        } else if (contents)
        {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return 0;
}

static int adapter_power_on(void)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(g_bus, BLUEZ, g_adapter_path, ADAPTER_IF,
                                "Powered", &err, "b", 1);
    if (r < 0)
        log_msg("Powered: %s", err.message ? err.message : "fail");
    sd_bus_error_free(&err);
    return r;
}

static int start_discovery(void)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g_bus, BLUEZ, g_adapter_path, ADAPTER_IF,
                               "StartDiscovery", &err, NULL, NULL);
    if (r < 0)
        log_msg("StartDiscovery: %s", err.message ? err.message : "fail");
    sd_bus_error_free(&err);
    return r;
}

static void stop_discovery(void)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    (void)sd_bus_call_method(g_bus, BLUEZ, g_adapter_path, ADAPTER_IF,
                             "StopDiscovery", &err, NULL, NULL);
    sd_bus_error_free(&err);
}

static int device_in_objects(const char *dev_path)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *rep = NULL;
    int found = 0;
    if (sd_bus_call_method(g_bus, BLUEZ, "/", OM_IF, "GetManagedObjects",
                           &err, &rep, NULL) < 0)
    {
        sd_bus_error_free(&err);
        return 0;
    }
    if (sd_bus_message_enter_container(rep, 'a', "{oa{sa{sv}}}") > 0)
    {
        while (sd_bus_message_enter_container(rep, 'e', "oa{sa{sv}}") > 0)
        {
            const char *opath = NULL;
            sd_bus_message_read(rep, "o", &opath);
            if (opath && strcmp(opath, dev_path) == 0)
                found = 1;
            sd_bus_message_skip(rep, "a{sa{sv}}");
            sd_bus_message_exit_container(rep);
        }
        sd_bus_message_exit_container(rep);
    }
    sd_bus_message_unref(rep);
    sd_bus_error_free(&err);
    return found;
}

static int device_connect(cli_t *c)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g_bus, BLUEZ, c->dev_path, DEVICE_IF,
                               "Connect", &err, NULL, NULL);
    if (r < 0)
    {
        log_msg("Connect %s: %s", c->mac, err.message ? err.message : "fail");
        sd_bus_error_free(&err);
        return -1;
    }
    sd_bus_error_free(&err);
    return 0;
}

static int services_resolved(cli_t *c)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int on = 0;
    int r = sd_bus_get_property_trivial(g_bus, BLUEZ, c->dev_path, DEVICE_IF,
                                        "ServicesResolved", &err, 'b', &on);
    sd_bus_error_free(&err);
    return r >= 0 && on;
}

static int start_notify(cli_t *c)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r;
    /* Accept notifies that race StartNotify. */
    c->acked = 1;
    if (c->notify_slot)
    {
        sd_bus_slot_unref(c->notify_slot);
        c->notify_slot = NULL;
    }
    r = sd_bus_match_signal(g_bus, &c->notify_slot, BLUEZ, c->notify_path,
                            PROPS_IF, "PropertiesChanged", on_notify, c);
    if (r < 0)
        log_msg("match_signal: %s", strerror(-r));
    r = sd_bus_call_method(g_bus, BLUEZ, c->notify_path, CHAR_IF,
                           "StartNotify", &err, NULL, NULL);
    if (r < 0)
    {
        log_msg("StartNotify: %s", err.message ? err.message : "fail");
        sd_bus_error_free(&err);
        return -1;
    }
    sd_bus_error_free(&err);
    (void)sd_bus_flush(g_bus);
    log_msg("StartNotify ok %s", c->notify_path);
    return 0;
}

static int on_device_props(sd_bus_message *m, void *userdata, sd_bus_error *ret_err)
{
    cli_t *c = userdata;
    const char *iface = NULL;
    (void)ret_err;
    if (!c || c->fd < 0 || (c->sess != SESS_READY && c->sess != SESS_WAIT_GATT))
        return 0;
    if (sd_bus_message_read(m, "s", &iface) < 0)
        return 0;
    if (!iface || strcmp(iface, DEVICE_IF) != 0)
        return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
        return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char *key = NULL;
        const char *contents = NULL;
        int on = 1;

        sd_bus_message_read(m, "s", &key);
        sd_bus_message_peek_type(m, NULL, &contents);
        if (key && strcmp(key, "Connected") == 0 && contents &&
            strcmp(contents, "b") == 0)
        {
            if (sd_bus_message_enter_container(m, 'v', "b") >= 0)
            {
                (void)sd_bus_message_read(m, "b", &on);
                sd_bus_message_exit_container(m);
            }
            if (!on)
            {
                sess_fail(c, "disconnected");
                return 0;
            }
        } else if (contents)
        {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    return 0;
}

static void watch_device(cli_t *c)
{
    if (!g_bus || !c || !c->dev_path[0])
        return;
    if (c->dev_slot)
    {
        sd_bus_slot_unref(c->dev_slot);
        c->dev_slot = NULL;
    }
    if (sd_bus_match_signal(g_bus, &c->dev_slot, BLUEZ, c->dev_path,
                            PROPS_IF, "PropertiesChanged",
                            on_device_props, c) < 0)
        log_msg("watch %s: match failed", c->mac);
}

static int on_if_removed(sd_bus_message *m, void *userdata, sd_bus_error *ret_err)
{
    const char *opath = NULL;
    int i;

    (void)userdata;
    (void)ret_err;
    if (sd_bus_message_read(m, "o", &opath) < 0 || !opath)
        return 0;
    for (i = 0; i < MAX_CLI; i++)
    {
        cli_t *c = &g_cli[i];
        size_t n;

        if (c->fd < 0 || !c->bound || !c->dev_path[0])
            continue;
        if (c->sess != SESS_READY && c->sess != SESS_WAIT_GATT)
            continue;
        n = strlen(c->dev_path);
        if (strcmp(opath, c->dev_path) == 0 ||
            (strncmp(opath, c->dev_path, n) == 0 && opath[n] == '/'))
        {
            sess_fail(c, "disconnected");
            break;
        }
    }
    return 0;
}

static int gatt_write(cli_t *c, const uint8_t *data, size_t n, int response)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r;
    if (!c->write_path[0])
        return -1;
    r = sd_bus_message_new_method_call(g_bus, &m, BLUEZ, c->write_path,
                                       CHAR_IF, "WriteValue");
    if (r < 0)
        return r;
    sd_bus_message_append_array(m, 'y', data, n);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "type");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", response ? "request" : "command");
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    r = sd_bus_call(g_bus, m, 0, &err, NULL);
    sd_bus_message_unref(m);
    if (r < 0)
    {
        const char *em = err.message ? err.message : "fail";
        log_msg("WriteValue: %s", em);
        if (is_link_lost(em))
        {
            sd_bus_error_free(&err);
            sess_fail(c, "disconnected");
            return -2;
        }
        sd_bus_error_free(&err);
        return -1;
    }
    sd_bus_error_free(&err);
    log_msg("WriteValue %s n=%zu", c->mac, n);
    return 0;
}

static void device_disconnect(cli_t *c)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (c->dev_path[0])
        (void)sd_bus_call_method(g_bus, BLUEZ, c->dev_path, DEVICE_IF,
                                 "Disconnect", &err, NULL, NULL);
    sd_bus_error_free(&err);
}

static void sess_fail(cli_t *c, const char *msg)
{
    int retry = c->bound && c->mac[0];

    log_msg("sess_fail %s: %s", c->mac[0] ? c->mac : "?", msg ? msg : "");
    if (g_scan_owner >= 0 && &g_cli[g_scan_owner] == c)
    {
        stop_discovery();
        g_scan_owner = -1;
    }
    queue_err(c, msg);
    sess_reset(c);
    c->acked = 0;
    if (retry)
        c->reconnect_at = mono_now() + RECONNECT_S;
}

static void sess_advance(cli_t *c, int idx)
{
    double now = mono_now();
    if (c->sess == SESS_SCAN)
    {
        if (device_in_objects(c->dev_path))
        {
            stop_discovery();
            if (g_scan_owner == idx)
                g_scan_owner = -1;
            c->sess = SESS_CONNECT;
            queue_state(c, "scanning", "found");
            if (device_connect(c) != 0)
            {
                sess_fail(c, "connect failed");
                return;
            }
            c->deadline = now + GATT_WAIT_S;
            c->sess = SESS_WAIT_GATT;
            return;
        }
        if (now > c->deadline)
        {
            stop_discovery();
            if (g_scan_owner == idx)
                g_scan_owner = -1;
            sess_fail(c, "connect timeout");
        }
        return;
    }
    if (c->sess == SESS_WAIT_GATT)
    {
        if (services_resolved(c) || now > c->deadline)
        {
            if (discover_ffe1(c) != 0)
            {
                sess_fail(c,
                          "GATT discovery returned no services. "
                          "Try: bluetoothctl remove <MAC> && systemctl restart bluetooth");
                return;
            }
            if (start_notify(c) != 0)
            {
                sess_fail(c, "StartNotify failed");
                return;
            }
            c->sess = SESS_READY;
            c->acked = 1;
            log_msg("connect ready %s", c->mac);
            queue_ok(c, "connect");
            queue_state(c, "connected", "");
        }
        return;
    }
}

static void collect_scan_results(char *out, size_t cap)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *rep = NULL;
    size_t off = 0;
    int first = 1;
    snprintf(out, cap, "{\"type\":\"scan\",\"results\":[");
    off = strlen(out);
    if (sd_bus_call_method(g_bus, BLUEZ, "/", OM_IF, "GetManagedObjects",
                           &err, &rep, NULL) < 0)
    {
        sd_bus_error_free(&err);
        snprintf(out + off, cap - off, "]}");
        return;
    }
    if (sd_bus_message_enter_container(rep, 'a', "{oa{sa{sv}}}") > 0)
    {
        while (sd_bus_message_enter_container(rep, 'e', "oa{sa{sv}}") > 0)
        {
            const char *opath = NULL;
            sd_bus_message_read(rep, "o", &opath);
            if (opath && strncmp(opath, g_adapter_path, strlen(g_adapter_path)) == 0 &&
                strstr(opath, "/dev_"))
            {
                char addr[32] = "", name[48] = "";
                int16_t rssi = 0;
                if (sd_bus_message_enter_container(rep, 'a', "{sa{sv}}") > 0)
                {
                    while (sd_bus_message_enter_container(rep, 'e', "sa{sv}") > 0)
                    {
                        const char *iface = NULL;
                        sd_bus_message_read(rep, "s", &iface);
                        if (iface && strcmp(iface, DEVICE_IF) == 0 &&
                            sd_bus_message_enter_container(rep, 'a', "{sv}") > 0)
                        {
                            while (sd_bus_message_enter_container(rep, 'e', "sv") > 0)
                            {
                                const char *key = NULL;
                                const char *contents = NULL;
                                sd_bus_message_read(rep, "s", &key);
                                sd_bus_message_peek_type(rep, NULL, &contents);
                                sd_bus_message_enter_container(rep, 'v', contents);
                                if (key && strcmp(key, "Address") == 0 && contents &&
                                    contents[0] == 's')
                                {
                                    const char *u = NULL;
                                    sd_bus_message_read(rep, "s", &u);
                                    if (u)
                                        snprintf(addr, sizeof(addr), "%s", u);
                                } else if (key && strcmp(key, "Name") == 0 &&
                                           contents && contents[0] == 's')
                                {
                                    const char *u = NULL;
                                    sd_bus_message_read(rep, "s", &u);
                                    if (u)
                                        snprintf(name, sizeof(name), "%s", u);
                                } else if (key && strcmp(key, "RSSI") == 0 &&
                                           contents && contents[0] == 'n')
                                {
                                    sd_bus_message_read(rep, "n", &rssi);
                                } else if (contents)
                                {
                                    sd_bus_message_skip(rep, contents);
                                }
                                sd_bus_message_exit_container(rep);
                                sd_bus_message_exit_container(rep);
                            }
                            sd_bus_message_exit_container(rep);
                        } else
                        {
                            sd_bus_message_skip(rep, "a{sv}");
                        }
                        sd_bus_message_exit_container(rep);
                    }
                    sd_bus_message_exit_container(rep);
                }
                if (addr[0] && off + 96 < cap)
                {
                    int n = snprintf(out + off, cap - off,
                                     "%s{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
                                     first ? "" : ",", addr, name[0] ? name : "?",
                                     (int)rssi);
                    if (n > 0)
                        off += (size_t)n;
                    first = 0;
                }
            } else
            {
                sd_bus_message_skip(rep, "a{sa{sv}}");
            }
            sd_bus_message_exit_container(rep);
        }
        sd_bus_message_exit_container(rep);
    }
    sd_bus_message_unref(rep);
    sd_bus_error_free(&err);
    snprintf(out + off, cap - off, "]}");
}

static void begin_connect(cli_t *c, int idx)
{
    c->reconnect_at = 0;
    if (!g_bus)
    {
        queue_err(c, "no system bus");
        return;
    }
    if (g_scan_owner >= 0 && g_scan_owner != idx)
    {
        /* serialize: wait; caller retries via sess_advance on owner */
        c->sess = SESS_SCAN;
        c->deadline = mono_now() + CONNECT_S;
        return;
    }
    adapter_power_on();
    mac_to_path(c->mac, c->dev_path, sizeof(c->dev_path));
    watch_device(c);
    if (device_in_objects(c->dev_path))
    {
        c->sess = SESS_CONNECT;
        if (device_connect(c) != 0)
        {
            sess_fail(c, "connect failed");
            return;
        }
        c->deadline = mono_now() + GATT_WAIT_S;
        c->sess = SESS_WAIT_GATT;
        return;
    }
    g_scan_owner = idx;
    c->sess = SESS_SCAN;
    c->deadline = mono_now() + CONNECT_S;
    queue_state(c, "scanning", "");
    if (start_discovery() < 0)
        sess_fail(c, "StartDiscovery failed");
}

static void handle_line(cli_t *c, int idx, const char *line)
{
    char cmd[32], addr[40], hex[HEX_CAP];
    cmd[0] = addr[0] = hex[0] = '\0';
    (void)json_str(line, "cmd", cmd, sizeof(cmd));
    (void)json_str(line, "address", addr, sizeof(addr));
    (void)json_str(line, "hex", hex, sizeof(hex));
    if (strcmp(cmd, "quit") == 0)
        exit(0);
    if (strcmp(cmd, "scan") == 0)
    {
        char body[LINE_CAP];
        double t = json_double(line, "timeout_s", SCAN_S);
        (void)t;
        if (g_bus)
        {
            adapter_power_on();
            start_discovery();
            /* brief wait: poll loop will keep running; return current objects */
            collect_scan_results(body, sizeof(body));
            stop_discovery();
            queue_line(c, body);
        }
        else
        {
            queue_line(c, "{\"type\":\"scan\",\"results\":[]}");
        }
        return;
    }
    if (strcmp(cmd, "connect") == 0)
    {
        if (!addr[0])
        {
            queue_err(c, "missing address");
            return;
        }
        if (mac_taken(addr, idx))
        {
            queue_err(c, "mac in use");
            return;
        }
        snprintf(c->mac, sizeof(c->mac), "%.*s", (int)sizeof(c->mac) - 1, addr);
        c->bound = 1;
        c->write_response = json_bool(line, "response", 0);
        c->acked = 0;
        begin_connect(c, idx);
        return;
    }
    if (strcmp(cmd, "write") == 0)
    {
        uint8_t raw[256];
        int n, resp, r;
        if (!c->acked || !c->bound)
        {
            queue_err(c, "handshake required");
            return;
        }
        if (addr[0] && !mac_eq(addr, c->mac))
        {
            queue_err(c, "address mismatch");
            return;
        }
        n = hex_decode(hex, raw, sizeof(raw));
        resp = json_bool(line, "response", c->write_response);
        r = gatt_write(c, raw, (size_t)n, resp);
        if (r == -2)
            return;
        if (r != 0)
            queue_err(c, "write failed");
        else
            queue_ok(c, "write");
        return;
    }
    if (strcmp(cmd, "disconnect") == 0)
    {
        if (addr[0] && c->mac[0] && !mac_eq(addr, c->mac))
        {
            queue_err(c, "address mismatch");
            return;
        }
        device_disconnect(c);
        sess_reset(c);
        c->acked = 0;
        c->bound = 0;
        c->reconnect_at = 0;
        queue_ok(c, "disconnect");
        c->mac[0] = '\0';
        return;
    }
}

static void cli_close(cli_t *c)
{
    int i;
    if (c->fd >= 0)
        close(c->fd);
    device_disconnect(c);
    sess_reset(c);
    for (i = 0; i < MAX_CLI; i++)
    {
        if (&g_cli[i] == c && g_scan_owner == i)
        {
            stop_discovery();
            g_scan_owner = -1;
        }
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void cli_flush(cli_t *c)
{
    while (c->out_off < c->out_len)
    {
        ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            cli_close(c);
            return;
        }
        c->out_off += (size_t)n;
    }
    c->out_len = c->out_off = 0;
}

static void cli_read(cli_t *c, int idx)
{
    for (;;)
    {
        ssize_t n;
        char *nl;
        if (c->in_len + 1 >= sizeof(c->in))
            c->in_len = 0;
        n = read(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            cli_close(c);
            return;
        }
        if (n == 0)
        {
            cli_close(c);
            return;
        }
        c->in_len += (size_t)n;
        c->in[c->in_len] = '\0';
        while ((nl = memchr(c->in, '\n', c->in_len)) != NULL)
        {
            size_t ln = (size_t)(nl - c->in);
            c->in[ln] = '\0';
            if (ln && c->in[ln - 1] == '\r')
                c->in[ln - 1] = '\0';
            handle_line(c, idx, c->in);
            memmove(c->in, nl + 1, c->in_len - ln - 1);
            c->in_len -= ln + 1;
            c->in[c->in_len] = '\0';
        }
    }
}

static int bind_abs(const char *adapter)
{
    int fd, n;
    struct sockaddr_un a;
    socklen_t alen;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';
    n = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "mf-gatt/%s", adapter);
    if (n < 0 || (size_t)n >= sizeof(a.sun_path) - 1)
    {
        close(fd);
        return -1;
    }
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    if (bind(fd, (struct sockaddr *)&a, alen) != 0 || listen(fd, 8) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "mf_gatt -- BlueZ GATT helper for Moon Flare\n"
            "Usage: %s [--adapter hci0] [--debug] [--help]\n"
            "Listens on abstract Unix socket @mf-gatt/<adapter>.\n"
            "JSON-lines: connect/write/disconnect/scan/quit.\n"
            "Does not speak JK framing.\n",
            prog);
}

int main(int argc, char **argv)
{
    int i, lfd, busfd = -1;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--adapter") && i + 1 < argc)
        {
            g_adapter = argv[++i];
        } else if (!strncmp(argv[i], "--adapter=", 10))
        {
            g_adapter = argv[i] + 10;
        } else if (!strcmp(argv[i], "--debug"))
        {
            g_debug = 1;
        } else
        {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    for (i = 0; i < MAX_CLI; i++)
        g_cli[i].fd = -1;
    snprintf(g_adapter_path, sizeof(g_adapter_path), "/org/bluez/%s", g_adapter);
    if (sd_bus_open_system(&g_bus) < 0)
    {
        log_msg("sd_bus_open_system failed (BlueZ commands will error)");
        g_bus = NULL;
    }
    if (g_bus &&
        sd_bus_match_signal(g_bus, NULL, BLUEZ, "/", OM_IF,
                            "InterfacesRemoved", on_if_removed, NULL) < 0)
        log_msg("InterfacesRemoved match failed");
    lfd = bind_abs(g_adapter);
    if (lfd < 0)
    {
        fprintf(stderr, "mf_gatt: bind @mf-gatt/%s: %s\n",
                g_adapter, strerror(errno));
        return 1;
    }
    printf("mf_gatt: listening on @mf-gatt/%s\n", g_adapter);
    fflush(stdout);
    if (g_bus)
        busfd = sd_bus_get_fd(g_bus);
    for (;;)
    {
        struct pollfd p[MAX_CLI + 2];
        int np = 0, ev;
        p[np].fd = lfd;
        p[np].events = POLLIN;
        np++;
        if (busfd >= 0)
        {
            ev = sd_bus_get_events(g_bus);
            p[np].fd = busfd;
            p[np].events = (short)(ev > 0 ? ev : POLLIN);
            np++;
        }
        for (i = 0; i < MAX_CLI; i++)
        {
            if (g_cli[i].fd < 0)
                continue;
            p[np].fd = g_cli[i].fd;
            p[np].events = POLLIN;
            if (g_cli[i].out_len > g_cli[i].out_off)
                p[np].events = (short)(p[np].events | POLLOUT);
            np++;
        }
        if (poll(p, (nfds_t)np, 50) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (g_bus)
        {
            int pr;
            do
            {
                pr = sd_bus_process(g_bus, NULL);
            } while (pr > 0);
        }
        if (p[0].revents & POLLIN)
        {
            int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0)
            {
                int slot = -1;
                for (i = 0; i < MAX_CLI; i++)
                {
                    if (g_cli[i].fd < 0)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                    close(fd);
                else
                {
                    memset(&g_cli[slot], 0, sizeof(g_cli[slot]));
                    g_cli[slot].fd = fd;
                }
            }
        }
        for (i = 0; i < MAX_CLI; i++)
        {
            if (g_cli[i].fd < 0)
                continue;
            cli_read(&g_cli[i], i);
            if (g_cli[i].fd < 0)
                continue;
            if (g_cli[i].bound && g_cli[i].mac[0] &&
                g_cli[i].sess == SESS_IDLE &&
                g_cli[i].reconnect_at > 0 &&
                mono_now() >= g_cli[i].reconnect_at)
            {
                log_msg("reconnect %s", g_cli[i].mac);
                begin_connect(&g_cli[i], i);
            }
            if (g_cli[i].sess == SESS_SCAN || g_cli[i].sess == SESS_WAIT_GATT)
                sess_advance(&g_cli[i], i);
            cli_flush(&g_cli[i]);
        }
        (void)g_debug;
    }
    close(lfd);
    if (g_bus)
        sd_bus_unref(g_bus);
    return 0;
}
