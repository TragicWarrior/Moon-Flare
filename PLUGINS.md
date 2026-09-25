# Writing a moon-flare plugin

This guide is for writing a new moon-flare module type, such as a battery, charger, inverter, sensor or web service. It assumes C and a Linux build toolchain. You need only one header, [`include/mf_plugin.h`](include/mf_plugin.h). A plugin never links against the daemon.

The guide ends with [a complete example plugin](#complete-example). It reports the host's load average, and it was built outside the source tree and loaded into a live daemon to check this guide.

## Contents

- [How it fits together](#how-it-fits-together)
- [Quick start](#quick-start)
- [Loading and naming](#loading-and-naming)
- [Kinds](#kinds)
- [The event loop](#the-event-loop)
- [The ops table](#the-ops-table)
- [Return codes](#return-codes)
- [Configuration: what `open()` receives](#configuration-what-open-receives)
- [`describe()`: the form and capabilities](#describe-the-form-and-capabilities)
- [Settings](#settings)
- [Readings](#readings)
- [History capture and pruning](#history-capture-and-pruning)
- [Actions](#actions)
- [Capabilities](#capabilities)
- [Probes (discovery)](#probes-discovery)
- [Files, processes and privileges](#files-processes-and-privileges)
- [Compatibility](#compatibility)
- [Testing a plugin](#testing-a-plugin)
- [Checklist](#checklist)
- [Complete example](#complete-example)

## How it fits together

- **`moonflared`** is the daemon. It runs as one thread and uses a `select()` loop. It loads every plugin at startup, holds the configuration, records history, and serves the REST API on port 5250.
- **A plugin** is a shared library, `libmf_<kind>_<driver>.so`. It provides one or more module types. Each type is identified by a *kind* (such as `battery`) and a *driver* (such as `jk`).
- **A module** is one configured instance of a module type. It has a name and a UUID. A user adds modules with **Modules → Add Module** in the TUI, or with `POST /api/v1/devices`. The daemon calls your `open()` once for each module. The `void *ctx` it returns holds that module's state, and every later call for that module gets it back.
- **Clients** are the TUI, `moonflare-cli` and the MCP server. They know nothing about your plugin. What they show comes from your `describe()`, your settings and your readings. A new plugin needs no client changes.

```
            REST :5250                          dlopen at startup
TUI/CLI/MCP ──────────► moonflared ──────────► libmf_<kind>_<driver>.so
                        │  select() loop            open()   one ctx per module
                        │  every tick:              step()   do I/O, never block
                        │                           get_reading()
                        ├─ config (/var/lib/moonflare/moonflared.json)
                        └─ history (/var/lib/moonflare/history/<uuid>.sqlite)
```

## Quick start

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2 -fPIC -shared \
   -I /path/to/moon-flare/include \
   -o libmf_service_loadavg.so loadavg.c

sudo install -m 0755 libmf_service_loadavg.so /usr/local/lib/moon-flare/
sudo systemctl restart moonflared      # plugins load only at startup
curl -s http://127.0.0.1:5250/api/v1/drivers   # your kind/driver is listed
```

Then add a module with **Modules → Add Module** in the TUI.

The daemon logs each plugin it loads, and each one it refuses, with the reason:

```
plugin /usr/local/lib/moon-flare/libmf_service_loadavg.so kind=service driver=loadavg ver=1.0.0
```

## Loading and naming

- **File name.** The daemon loads every `libmf_*.so` in its plugin directory. The directory is `/usr/local/lib/moon-flare` by default, or whatever `--plugin-dir` gives. Name yours `libmf_<kind>_<driver>.so`.
- **Entry point.** Export exactly one symbol:

  ```c
  size_t mf_plugin_entries(const mf_plugin_ops_t **out);
  ```

  Point `*out` at an array of `N` consecutive `mf_plugin_ops_t` and return `N`. Most plugins return 1. One library may provide several module types; `libmf_demo.so` provides a battery and a charger. Keep every other symbol `static`, or build with `-fvisibility=hidden`.
- **The loader refuses a library** when any entry has `abi != MF_PLUGIN_ABI`, or an `ops_size` smaller than `MF_PLUGIN_OPS_MIN_SIZE`, or entries of different sizes. Always set `.abi = MF_PLUGIN_ABI` and `.ops_size = sizeof(mf_plugin_ops_t)`.
- **Names.** `kind` and `driver` are each at most 15 characters. Use lowercase `[a-z0-9_]`. The kind/driver pair must be unique across every installed plugin: the first match wins.
- **Limits.** The daemon accepts at most 16 plugin libraries, 32 module types and 32 modules.
- **Loading is at startup only.** Restart the daemon after installing or upgrading a plugin.

## Kinds

The kind decides where a module appears on the dashboard, and which reading keys the daemon reads out.

| Kind | Dashboard | What the daemon reads from your reading |
| --- | --- | --- |
| `battery` | Batteries frame, pack view, system totals | `pack_voltage_v`, `current_a` (negative while discharging), `soc_pct`, `full_capacity_ah`, `remaining_capacity_ah`, `cell_count` |
| `charger` | Chargers frame, charger view, the Input meter | `battery_voltage_v`, `charging_watts`, `kwh_today`, `charge_stage` |
| `inverter` | Inverters frame (a list for now) | nothing yet |
| `service` | Services frame; the whole reading is passed through as `data` | the Info panel shows a `weather` object (see [Readings](#readings)) |
| `actuator` | Actuators frame; the whole reading is passed through as `data` | nothing yet |

Any other kind still works through `/api/v1/devices`, settings, actions and history, but it gets no place on the dashboard.

The driver name `phantom` is taken: moonflared has a built-in phantom driver for `battery`, `charger` and `inverter`, which averages the readings of real modules it shadows (see the README's *Phantom modules*).

- **Battery and charger** modules count toward the system totals, but only while they are both online and marked active.
- **The pack view** also shows these keys when present:
  - `cells`: an array of `{"index", "voltage_v", "balancing"}`
  - `temperatures_c`: an array of numbers
  - `temp_labels`: an array of strings
  - `soh_pct`
  - `charge_mosfet_on`, `discharge_mosfet_on`, `balancer_switch`: booleans
- **The charger view** also shows `ah_today`.

## The event loop

Everything runs on the daemon's single thread. **A plugin must never block.** If a plugin sleeps, makes a blocking `read()` or `connect()`, or waits on a synchronous HTTP request, it freezes polling, the REST API and every other module until it returns.

Each pass of the main loop does the following:

1. **Collect descriptors.** For every module, the daemon calls `prepare_fds(ctx, r, w, &maxfd)` if you provide it. Use that for several descriptors, as with libcurl's multi interface. Otherwise it calls `fd(ctx)` and `select_mask(ctx)`: return `-1` when you have no descriptor, and the bits `MF_IO_WANT_READ` and `MF_IO_WANT_WRITE`.
2. **Wait.** It runs `select()` with a **50 ms** timeout. Your descriptors wake the loop early.
3. **Step every module.** It calls `step(ctx)` **on every tick**, whether or not your descriptor is ready. Do non-blocking I/O here, and keep your own schedule, such as "read every `poll_interval_s` seconds", with `CLOCK_MONOTONIC`. Return:
   - `MF_STEP_IDLE`: nothing new.
   - `MF_STEP_UPDATED`: there is new data.
   - `MF_STEP_ERROR`: the module is offline. The daemon calls `last_error(ctx)` for the reason shown to users.
4. **Read out.** After every step that is not an error, the daemon calls `get_reading(ctx, …)`. That happens up to 20 times a second, so keep it cheap: format what you already have. Do no I/O there.

A device that talks all the time, like the Magnum inverter bus ([`src/plugins/inverter_magnum/`](src/plugins/inverter_magnum/plugin.c)), fits the same loop. Return its port from `fd()` with `MF_IO_WANT_READ`, read until `EAGAIN` in each `step()`, and rebuild the reading on your own schedule. The kernel buffers whatever arrives between steps.

`open()` and `close()` also run on the main thread. `open()` often runs inside an HTTP request, when a user adds a module. Both must return quickly. In `open()`, parse the configuration, allocate your context and start non-blocking work. Do the actual connecting in `step()`.

The daemon calls `close(ctx)` when:
- the module is removed or disabled;
- its transport identity changes (bus, USB path, BLE address, Modbus IP or port), after which it calls `open()` again with the new configuration;
- the daemon shuts down.

Free everything in `close()`, and kill any helper processes you started.

Multiple modules of the same type get separate contexts. **Keep all state in the context, not in globals.**

## The ops table

Fields marked "may be NULL" are optional. The rest are required: a NULL there makes the feature fail or the module never produce data.

| Field | Contract |
| --- | --- |
| `abi` | `MF_PLUGIN_ABI`. |
| `ops_size` | `sizeof(mf_plugin_ops_t)`. |
| `kind`, `driver` | Static strings; see [Loading and naming](#loading-and-naming). |
| `version` | Your plugin's version, shown in the daemon log. |
| `open(spec_json, err, errsz)` | Create one module's context from its configuration (see [Configuration](#configuration-what-open-receives)). Return NULL and write a reason into `err` to refuse: a bad setting, or a missing required key. Adding the module then fails with HTTP 400 and your message. |
| `close(ctx)` | Free the context. |
| `fd(ctx)`, `select_mask(ctx)` | One descriptor to wait on, and whether to wait for read or write. Return `-1` and `0` when you have none. |
| `prepare_fds(ctx, r, w, &maxfd)` | May be NULL. Add any number of descriptors to the sets and raise `*maxfd`. When present, it is used instead of `fd` and `select_mask`. |
| `step(ctx)` | Advance your state machine; see [The event loop](#the-event-loop). |
| `caps(ctx)` | A bitmask of `MF_CAP_*`; see [Capabilities](#capabilities). **Also called with `ctx == NULL`** when the daemon lists drivers. |
| `last_error(ctx)` | A short human-readable reason for the last failure, or NULL. |
| `get_reading(ctx, json, cap)` | Write the module's current data as one JSON object; see [Readings](#readings). |
| `get_settings(ctx, json, cap)` | May be NULL. Write your settings as one flat JSON object; see [Settings](#settings). |
| `put_settings(ctx, json, err, errsz)` | May be NULL. Apply changed settings. |
| `action(ctx, name, json, err, errsz)` | May be NULL. Perform a named action; see [Actions](#actions). |
| `probe_*` | May be NULL; see [Probes](#probes-discovery). |
| `describe()` | May be NULL, but provide it. It returns static JSON: the Add Module form, the bus, and history capture. See [`describe()`](#describe-the-form-and-capabilities). |

## Return codes

| Code | Meaning | HTTP status from settings and actions |
| --- | --- | --- |
| `MF_OK` (0) | Success. | 200 |
| `MF_ERR_INVAL` | Bad input; put the reason in `err`. | 400 |
| `MF_ERR_UNSUPPORTED` | Unknown action or setting. | 400 |
| `MF_ERR_OFFLINE` | The hardware or service is unreachable. From `get_reading`, it means there is nothing to show yet. | 409 |
| `MF_ERR_BUSY` | Try again later. | 409 |

## Configuration: what `open()` receives

The daemon owns each module's configuration. It is kept in the state file, `/var/lib/moonflare/moonflared.json`. `open()` receives the module's entry from that file as JSON:

```json
{"uuid": "d9b200fd-…", "name": "Load", "kind": "service", "driver": "loadavg",
 "enabled": true, "active": true, "poll_interval_s": 1,
 "capture_interval_s": -1, "retention_days": -1, "bus": null,
 "usb": {…}, "ble": {…}, "modbus": {…},
 "loadavg": {"window": 5}}
```

- **Your own keys arrive nested.** A field declared in `describe()` as `loadavg.window` arrives as `{"loadavg": {"window": 5}}`. Use one prefix of your own for all your keys, typically your driver name.
- **Values may be strings.** A value typed into a form is saved as the user entered it, such as `"15"`. Accept both numbers and numeric strings.
- **Built-in transports.** `usb.*` (`path`, `serial_id`, `baud`, `addr`, `auto_port`), `ble.*` (`address`, `adapter`, `protocol`, `password`) and `modbus.*` (`ip`, `port`, `unit_id`, `auto_net`) are the daemon's own transport keys.
  - Use them if your hardware sits on one of those buses.
  - Together with `bus`, they form the module's identity. Two modules may not share an endpoint, and changing the endpoint reopens the module.
  - The TUI shows them read-only once a module exists.
- **`poll_interval_s` is yours to honour.** The daemon stores and validates it, and never lets it go below 0.5 s, but your `step()` does the scheduling.
- **Daemon-owned keys:** `capture_interval_s`, `retention_days` and `active` are handled by the daemon. Ignore them.
- **A plugin never writes the configuration.** The daemon saves settings changes itself; see [Settings](#settings).

## `describe()`: the form and capabilities

`describe()` returns one static JSON string, used for as long as the plugin is loaded:

```json
{"bus": "usb-serial",
 "fields": [
   {"key": "poll_interval_s", "label": "Poll Interval", "hint": "(Seconds)",
    "type": "number", "default": 2.0},
   {"key": "usb.path", "label": "USB Path", "hint": "(device)",
    "type": "string", "default": "/dev/ttyUSB0", "required": true}],
 "capture": {…}}
```

- **`fields`** is the Add Module form, in order. It is also the set of settings the daemon saves (see [Settings](#settings)).
  - `key` is a dotted configuration path.
  - `type` is `string`, `number`, `bool` or `modules`. A `modules` field holds a comma-separated list of module UUIDs of the same kind as the module; the TUI shows it by name and edits it as a checklist (the built-in phantom driver's Shadows use it).
  - `label`, `hint`, `default` and `required` are optional.
  - Labels and hints also caption the settings form. Keep a hint to about 12 characters or it is clipped.
  - Include `poll_interval_s` if your module polls.
- **`bus`** is optional. It is a short transport name stored with the module, such as `usb-serial`, `ble` or `modbus-tcp`.
- **`capture`** is optional. It declares history recording; see [History capture and pruning](#history-capture-and-pruning).
- **No `describe()`** means the form asks only for a name and a poll interval.

`GET /api/v1/drivers` passes `bus`, `fields` and `capture` through to clients.

## Settings

A module's settings form combines the daemon's own settings (name, poll and capture intervals, keep history, active) with yours from `get_settings`.

- **`get_settings(ctx, json, cap)`** writes one flat object. Use the same dotted keys as your `describe()` fields: `{"loadavg.window": 5}`. Every scalar becomes a field. Leave out the daemon's own keys; they are dropped anyway.
- **`put_settings(ctx, json, err, errsz)`** receives the client's JSON body as it was sent: flat dotted keys, possibly mixed with daemon keys such as `poll_interval_s`.
  - Honour `poll_interval_s` if it is present.
  - **Validate every key before changing anything.** Returning an error must leave the module unchanged.
  - Return `MF_ERR_INVAL` with a reason for bad values.
  - The daemon calls it only when the body has at least one key that isn't the daemon's.
- **Persistence.** After a successful PUT, the daemon saves only the keys your `describe()` declares as fields. Other keys are treated as live device writes: they reach the device but are not saved and not replayed on restart. A BMS protection register is an example. That split is deliberate: configuration keys go in `fields`, and live device settings go only through `get_settings` and `put_settings`.

## Readings

`get_reading(ctx, json, cap)` writes the module's data as **one JSON object**. The daemon wraps it with the id, name, online state and sequence number.

- **When there's no data yet** (the first poll hasn't completed), return `MF_ERR_OFFLINE`.
- **Size.** `cap` is 16 KB. Stay under **4 KB** if the module records history. A larger reading still reaches clients, but its history samples are stored as `{}`, with every declared column empty.
- **Types and units.** Use numbers, not strings. Put units in key suffixes: `_v`, `_a`, `_w`, `_wh`, `_ah`, `_pct`, `_c`, `_f`, `_s`. Use `null` for a value a device omits. For batteries and chargers, use the key names in [Kinds](#kinds) so the dashboard and system totals pick them up.
- **Weather.** A `service` that publishes this object feeds the dashboard's Info panel, whichever provider it uses:

  ```json
  {"weather": {"temp_f": 84.2, "conditions": "Clear", "icon": "clear",
               "is_day": true, "humidity_pct": 62, "wind_mph": 9.2,
               "wind_dir": "S", "station": "KDWH", "place": "Tomball, TX",
               "observed_local": "10:53",
               "forecast": [{"name": "Tonight", "temp_f": 72,
                             "short": "Mostly Clear", "icon": "clear",
                             "is_day": false}]}}
  ```

  `icon` is one of: `clear`, `partly_cloudy`, `mostly_cloudy`, `cloudy`, `wind`, `rain`, `showers`, `thunderstorm`, `snow`, `blizzard`, `sleet`, `freezing_rain`, `fog`, `haze`, `tornado`, `hurricane`, `hot`, `cold`. The dashboard picks the symbol.

## History capture and pruning

Add a `capture` block to `describe()` and the daemon records the module's history in its own SQLite file, `/var/lib/moonflare/history/<uuid>.sqlite`:

```json
"capture": {"interval_s": 60, "min_s": 5, "retention_days": 60, "graph": "one",
            "columns": {"one": "load.one",
                        "station": {"path": "weather.station", "type": "text"}}}
```

| Key | Meaning |
| --- | --- |
| `interval_s` | The default capture interval in seconds. 0 means off until the user sets one. |
| `min_s` | The shortest interval a user may set. Default 1. |
| `retention_days` | **Required.** The default pruning policy in whole days; 0 keeps history forever. A capture block without it is rejected: the daemon logs a warning and the module records nothing. |
| `columns` | Maps a column name to a dotted path in your reading. Names are `[a-z0-9_]`, at most 31 characters, and at most 16 columns. Columns are numeric unless given as `{"path": …, "type": "text"}`. |
| `graph` | The numeric column the TUI graphs. |

- **The whole reading is always stored too.** A column added in a later version of your plugin is added to existing files automatically, and old rows can be backfilled from the stored JSON.
- **Pruning.** Each module prunes its own file hourly, deleting samples older than its policy.
- **User settings.** The user can change the capture interval and the retention for each module, in the settings form (Capture Interval and Keep History) or through the REST API.
- **Plugin responsibilities.** A plugin only declares the spec; the daemon does the storage and pruning. Without a `capture` block, the module records nothing and has neither setting.

## Actions

`POST /api/v1/devices/{id}/actions/{name}` with a JSON body calls `action(ctx, name, json, err, errsz)`. After a success, the daemon takes a fresh reading.

Two conventions:

- **`set_switch`** with `{"key": "charge", "value": true}`: turn a named switch on or off. The TUI's pack view sends it for `charge`, `discharge` and `balance`.
- **`refresh`** (no body): poll now instead of waiting for the interval. No client sends it automatically today, but scripts and the REST API can.

Return `MF_ERR_UNSUPPORTED` for names you don't know. Advertise what you support with [capabilities](#capabilities).

## Capabilities

`caps(ctx)` returns `MF_CAP_*` bits. Clients receive them as strings in `caps`.

| Bit | String | Meaning |
| --- | --- | --- |
| `MF_CAP_READ` | `read` | Produces readings. Set it. |
| `MF_CAP_WRITE_SETTINGS` | `write_settings` | `put_settings` changes something. |
| `MF_CAP_ACTION_SWITCH` | `action.switch` | Supports `set_switch`. |
| `MF_CAP_ACTION_REFRESH` | `action.refresh` | Supports `refresh`. |
| `MF_CAP_PROBE` | `probe` | Has a probe. |
| `MF_CAP_AUTO_PORT` | `auto_port` | Finds its USB device again if the port changes. |
| `MF_CAP_AUTO_NET` | `auto_net` | Finds its device on the network. |

The daemon adds `history` itself when your `describe()` has a valid `capture` block.

`caps` is called with `ctx == NULL` when drivers are listed, and with your context for a live module. Your answer may differ between the two, for example once you have learned the firmware's features.

## Probes (discovery)

The `probe_*` functions run a one-off non-blocking discovery job behind `POST /api/v1/discover`. The call sequence is:

1. `probe_start` creates the job.
2. `probe_prepare_fds` and `probe_step` are called each tick until `probe_step` returns `MF_STEP_UPDATED` (done) or `MF_STEP_ERROR`.
3. `probe_result` writes the result JSON.
4. `probe_close` frees the job.

Discovery currently routes each bus to a built-in driver: Modbus to `classic`, USB to `xd`, BLE to `jk`. A third-party probe is reached only when a request names no bus. **Set all `probe_*` fields to NULL** unless you need this.

## Files, processes and privileges

- **User and groups.** The daemon runs as the user in `contrib/moonflared.service`, with the groups `dialout` (serial) and `bluetooth`. It does not run as root.
- **State directory.** Keep persistent files under `$STATE_DIRECTORY/<driver>/`, falling back to `/var/lib/moonflare/<driver>/`. Create the directory yourself, and write files atomically (write a temporary file, then rename it).
- **Helper processes.** You may `fork()` or `exec()` a helper and talk to it over a pipe. JK does this with `mf_gatt` for BLE. The daemon reaps children with `waitpid(-1, …, WNOHANG)` and ignores `SIGPIPE`. Kill your helper in `close()`.
- **Threads.** Prefer none; the main loop does the scheduling. If you must use a thread, never call back into the daemon from it, and hand results over through your context with proper locking.
- **Libraries.** Link your dependencies into your plugin: static, or ordinary shared libraries. The weather.gov plugin links libcurl and a static cJSON. Avoid global state in them that could clash with another plugin.

## Compatibility

`MF_PLUGIN_ABI` changes only for an incompatible ABI change. New optional fields are added at the end of `mf_plugin_ops_t`.

The loader copies your table into a zeroed full-size slot. So a plugin built against an older header, and therefore with a smaller `ops_size`, still loads, and the fields it lacks read as NULL. Rebuild against a newer header to use its new fields.

## Testing a plugin

Run a private daemon on another port, with a scratch configuration, so nothing touches a live install:

```sh
mkdir -p /tmp/mf-try/plugins && cp libmf_service_loadavg.so /tmp/mf-try/plugins/
echo '{"history":{"dir":"/tmp/mf-try/history","path":"/tmp/mf-try/old.sqlite"},"devices":[]}' \
    > /tmp/mf-try/cfg.json
MF_STATE_CONFIG=/tmp/mf-try/state.json moonflared --foreground \
    --listen 127.0.0.1:5399 --plugin-dir /tmp/mf-try/plugins --config /tmp/mf-try/cfg.json
```

Then, in another terminal:

```sh
H=http://127.0.0.1:5399/api/v1
curl -s $H/drivers                                    # form, caps, capture spec
curl -s -XPOST -d '{"name":"Load","kind":"service","driver":"loadavg","loadavg.window":5}' $H/devices
curl -s $H/status                                     # your reading under its kind
curl -s $H/devices/<id>/settings                      # yours merged with the daemon's
curl -s -XPUT -d '{"loadavg.window":15}' $H/devices/<id>/settings
curl -s -XPOST -d '{}' $H/devices/<id>/actions/refresh
curl -s $H/devices/<id>/history                       # after one capture interval
moonflare-tui --connect 127.0.0.1:5399
```

Also run it under `valgrind --leak-check=full` for a while, and cover adding and removing modules. Every `open()` needs a matching `close()`.

## Checklist

- [ ] One exported symbol, `mf_plugin_entries`; `abi` and `ops_size` set.
- [ ] Nothing blocks; `step()` keeps its own schedule; `get_reading()` does no I/O.
- [ ] All state is in the context; `close()` frees it and stops any helpers.
- [ ] `open()` validates the configuration and refuses with a clear `err`.
- [ ] `caps(NULL)` works.
- [ ] Settings use dotted keys that match `describe()`; `put_settings` validates everything before it applies anything, and accepts numeric strings.
- [ ] The reading is one JSON object, with numbers as numbers and units in the key names; it is under 4 KB if the module captures history.
- [ ] If the module captures history, `capture` has `retention_days`, and its column paths match the reading.
- [ ] `probe_*` is NULL unless needed.

## Complete example

This is `libmf_service_loadavg.so`: a `service` that reads `/proc/loadavg` every poll interval. It has one setting of its own (which average to report), records history, and supports `refresh`. It is built with the command in [Quick start](#quick-start). Before this guide was written, it was loaded into a live daemon, and each of its functions checked through the REST API: adding, rejecting a bad setting, readings, settings, actions, history and removal.

```c
/* libmf_service_loadavg.so: the host's load average as a moon-flare service.
 * Build: cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2 -fPIC \
 *            -shared -I<moon-flare>/include -o libmf_service_loadavg.so loadavg.c */

#include "mf_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    double poll_s;          /* how often to read /proc/loadavg */
    int    window;          /* 1, 5 or 15 minutes: which average to report */
    double next;            /* monotonic time of the next read */
    int    have;            /* a reading exists */
    double load[3];
    char   err[96];
} la_ctx_t;

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Minimal lookups for this example; a real plugin should use a JSON parser
 * (moon-flare's own plugins link cJSON statically). */
static int find_num(const char *json, const char *key, double *out)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = json ? strstr(json, pat) : NULL;
    if (!p || !(p = strchr(p + strlen(pat), ':')))
        return 0;
    p++;
    while (*p == ' ' || *p == '"')      /* numbers may arrive as strings */
        p++;
    if (!(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')))
        return 0;
    *out = strtod(p, NULL);
    return 1;
}

static int valid_window(double w)
{
    return w == 1.0 || w == 5.0 || w == 15.0;
}

static void *la_open(const char *spec_json, char *err, size_t errsz)
{
    la_ctx_t *c = calloc(1, sizeof(*c));
    const char *own;
    double v;

    if (!c)
    {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    c->poll_s = 5.0;
    c->window = 1;
    if (find_num(spec_json, "poll_interval_s", &v) && v >= 1.0)
        c->poll_s = v;
    /* Plugin keys arrive nested: "loadavg.window" -> {"loadavg":{"window":5}} */
    own = spec_json ? strstr(spec_json, "\"loadavg\"") : NULL;
    if (own && find_num(own, "window", &v))
    {
        if (!valid_window(v))
        {
            snprintf(err, errsz, "loadavg.window must be 1, 5 or 15");
            free(c);
            return NULL;
        }
        c->window = (int)v;
    }
    return c;
}

static void la_close(void *ctx)
{
    free(ctx);
}

/* No descriptor: step() runs every daemon tick (50 ms) and keeps time. */
static int la_fd(void *ctx)
{
    (void)ctx;
    return -1;
}

static unsigned la_mask(void *ctx)
{
    (void)ctx;
    return 0;
}

static mf_step_t la_step(void *ctx)
{
    la_ctx_t *c = ctx;
    FILE *f;
    double t = now_mono();

    if (t < c->next)
        return MF_STEP_IDLE;
    c->next = t + c->poll_s;
    f = fopen("/proc/loadavg", "r");    /* a proc file never blocks */
    if (!f || fscanf(f, "%lf %lf %lf", &c->load[0], &c->load[1],
                     &c->load[2]) != 3)
    {
        if (f)
            fclose(f);
        snprintf(c->err, sizeof(c->err), "cannot read /proc/loadavg");
        return MF_STEP_ERROR;
    }
    fclose(f);
    c->have = 1;
    c->err[0] = '\0';
    return MF_STEP_UPDATED;
}

static unsigned la_caps(void *ctx)
{
    (void)ctx;                          /* NULL when asked by GET /drivers */
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS;
}

static const char *la_last_error(void *ctx)
{
    la_ctx_t *c = ctx;

    return c->err[0] ? c->err : NULL;
}

static int la_get_reading(void *ctx, char *json, size_t cap)
{
    la_ctx_t *c = ctx;
    double w = c->load[c->window == 1 ? 0 : c->window == 5 ? 1 : 2];
    int n;

    if (!c->have)
        return MF_ERR_OFFLINE;          /* nothing to show yet */
    n = snprintf(json, cap,
                 "{\"load\":{\"one\":%.2f,\"five\":%.2f,\"fifteen\":%.2f,"
                 "\"window_min\":%d,\"value\":%.2f}}",
                 c->load[0], c->load[1], c->load[2], c->window, w);
    return n > 0 && (size_t)n < cap ? MF_OK : MF_ERR_INVAL;
}

/* Settings use the same dotted keys as describe()'s fields. */
static int la_get_settings(void *ctx, char *json, size_t cap)
{
    la_ctx_t *c = ctx;
    int n = snprintf(json, cap, "{\"loadavg.window\":%d}", c->window);

    return n > 0 && (size_t)n < cap ? MF_OK : MF_ERR_INVAL;
}

static int la_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    la_ctx_t *c = ctx;
    double v;

    /* Validate everything before changing anything. */
    if (find_num(json, "loadavg.window", &v) && !valid_window(v))
    {
        snprintf(err, errsz, "loadavg.window must be 1, 5 or 15");
        return MF_ERR_INVAL;
    }
    if (find_num(json, "loadavg.window", &v))
        c->window = (int)v;
    /* The daemon owns poll_interval_s but passes it along: honour it. */
    if (find_num(json, "poll_interval_s", &v) && v >= 1.0)
        c->poll_s = v;
    return MF_OK;
}

static int la_action(void *ctx, const char *action, const char *json,
                     char *err, size_t errsz)
{
    la_ctx_t *c = ctx;

    (void)json;
    if (strcmp(action, "refresh") == 0)
    {
        c->next = 0.0;                  /* read on the next tick */
        return MF_OK;
    }
    snprintf(err, errsz, "unknown action: %s", action);
    return MF_ERR_UNSUPPORTED;
}

static const char *la_describe(void)
{
    return
        "{\"fields\":["
        "{\"key\":\"poll_interval_s\",\"label\":\"Poll Interval\","
        "\"hint\":\"(Seconds)\",\"type\":\"number\",\"default\":5},"
        "{\"key\":\"loadavg.window\",\"label\":\"Window\","
        "\"hint\":\"(1/5/15 min)\",\"type\":\"number\",\"default\":1}"
        "],"
        "\"capture\":{\"interval_s\":60,\"min_s\":5,\"retention_days\":60,"
        "\"graph\":\"one\",\"columns\":{"
        "\"one\":\"load.one\",\"five\":\"load.five\","
        "\"fifteen\":\"load.fifteen\"}}}";
}

static const mf_plugin_ops_t g_ops = {
    .abi          = MF_PLUGIN_ABI,
    .ops_size     = sizeof(mf_plugin_ops_t),
    .kind         = "service",
    .driver       = "loadavg",
    .version      = "1.0.0",
    .open         = la_open,
    .close        = la_close,
    .fd           = la_fd,
    .select_mask  = la_mask,
    .step         = la_step,
    .caps         = la_caps,
    .last_error   = la_last_error,
    .get_reading  = la_get_reading,
    .get_settings = la_get_settings,
    .put_settings = la_put_settings,
    .action       = la_action,
    .describe     = la_describe,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    *out = &g_ops;
    return 1;
}
```
