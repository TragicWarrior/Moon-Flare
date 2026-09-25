# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.11.2] - 2026-09-25

### Fixed

- When the libviper checkout is too old, the configure message also says
  to check that `git pull` succeeded: before libviper 7.8.2 each build
  rewrote the tracked `libviper.pc`, so the next pull stopped on that
  change, and the build that followed rebuilt the old libviper. It gives
  the command that clears it (`git checkout -- libviper.pc`).

## [0.11.1] - 2026-09-25

Building and installing on other hosts.

### Fixed

- The TUI reads libviper's headers from the top of the libviper checkout,
  laid out as an installed libviper has them, instead of through its
  `vdk/` and `vkmio/` links. Those links pointed into one machine's
  checkout (libviper 7.8.1 makes them relative), so on any other host the
  compiler fell back to whatever `vdk.h` was in `/usr/local/include`, and
  an old one there failed the build with dozens of API errors.
- Configuring checks the libviper checkout, and stops with the commands to
  run when it is missing, older than 7.8.0, not built, or not rebuilt
  since its last update. A libviper built in a `build/` directory inside
  its checkout works too.
- The daemon links the system's SQLite: `libsqlite3-dev` when installed,
  else the `libsqlite3.so.0` runtime with the bundled header. It linked the
  bundled x86-64 library, which failed on ARM and other hosts; that
  library is now only a last resort on x86-64.
- The systemd unit is generated at configure time. `ExecStart` follows
  `CMAKE_INSTALL_PREFIX`; `User=` is `-DMF_SERVICE_USER` (default: whoever
  configured the build) instead of a fixed account; `SupplementaryGroups=`
  lists whichever of `dialout` (or `uucp`) and `bluetooth` the system has
  (`-DMF_SERVICE_GROUPS` overrides), so the unit starts on distributions
  without them.
- The daemon's default `plugin_dir` and `gatt_bin`, and the JK plugin's
  `mf_gatt` path, follow `CMAKE_INSTALL_PREFIX` instead of assuming
  `/usr/local`. The example `moonflared.json` no longer sets them.
- `cmake --install` puts a copy of libvdk in the plugin directory and
  points the installed `moonflare-tui` at it, so the TUI runs without
  libviper installed system-wide. It installs `moonflare-cli` too.

### Changed

- The README lists the build prerequisites and how to clone and build
  libviper beside Moon Flare, and uses the TUI's current name,
  `moonflare-tui`. The example `moonflare.json` no longer carries a site's
  daemon address.

## [0.11.0] - 2026-09-25

### Added

- The TUI's Discharge meter scale is now a display preference, chosen in
  File → General:
  - **Auto** (the default): the highest discharge seen from the daemon,
    in 500 W steps, remembered between runs in
    `~/.local/state/moonflare/tui-state.json`, with a Reset Peak button;
  - **Battery limits**: what the counted packs can deliver;
  - **Inverter ratings**: what the counted inverters are rated for;
  - **Fixed watts**: a value you enter.

  A line under the choice says what it gives right now. Unknown limits,
  and a fixed scale without watts, use Auto. File → Save config keeps
  the choice (`discharge_scale`, `discharge_scale_w` in `moonflare.json`).
- `/api/v1/status` has `system.battery_limit_w` (the counted packs'
  maximum discharge current times voltage, summed) and
  `system.inverter_rated_w` (the counted inverters' ratings, summed), plus
  `inverters_counted` and `inverters_total`. Each is `null` unless every
  counted pack or inverter reports its figure.
- The JK battery reading has `max_discharge_a`, from its BMS settings
  frame. The Magnum inverter reading has `rated_w`, from the model (an
  MS4448PAE is 4400 W). The demo battery reports a 100 A limit.
- Sunrise and sunset in the weather reading (`weather.sunrise` and
  `weather.sunset`, local `HH:MM`). They come from weather.gov's `/points`
  reply (its `astronomicalData`), which the plugin now fetches again just
  after local midnight, so the times are always today's.

### Fixed

- The Capacity meter (and a pack's SOC on the dashboard) jumped around,
  e.g. from 82% to 49% and back. The LFP voltage check, added for the JK
  pack's drifting coulomb counter, applied to every pack: whenever one
  reading showed 0 A (an XD reports 0.00 A when solar roughly matches the
  load), it replaced the BMS's SOC with a voltage estimate that reads LFP's
  flat middle range far too low. It now applies to JK packs only, for now
  (until that pack's cells are top-balanced); every other pack's SOC is
  its BMS's.

### Changed

- The Discharge meter reads `3508 W (above 3000 W)`, not `3508 W / 3000 W`,
  when a fixed or known scale is exceeded.
- The dashboard's Info panel shows sunrise and sunset in place of humidity
  and wind. The next two forecast periods share one line, each with its
  icon and named by what it is: `Day 92F  Night 72F` (`Night 72F  Day 91F`
  in the evening). On an 80-column screen the line drops the `F` units to
  keep the icons. Humidity and wind remain in the reading for other
  clients.
- When weather.gov's icon doesn't say whether it's day or night, the
  weather plugin decides by sunrise and sunset instead of a fixed 6:00 to
  19:00.

## [0.10.0] - 2026-09-25

### Added

- A Textbelt SMS service (`libmf_service_textbelt.so`, kind `service`,
  driver `textbelt`), the first notification pathway.
  - **Settings:** the API key, a cap on texts an hour (default 20), and how
    often to check credits. It keeps no recipients: whoever raises an alert
    says whom it goes to.
  - **Send Test SMS**, a row in its settings, texts a number you give and
    shows how it went.
  - **Credits left** come from every reply from Textbelt and from a regular
    check. They show on the dashboard's Services card, in
    `moonflare-cli --status` and `-q`, and in its settings.
  - **Actions:** `test` (`{"to"}`), `notify` (`{"message", "title", "to"}`,
    `title` optional) and `refresh`.
  - Built only when libcurl is present, like the weather plugin.
- Notification pathways, so a logic engine (planned) can send alerts
  through any module that delivers messages:
  - a new capability, `MF_CAP_NOTIFY` (`"notify"` in `caps`);
  - a `notify` block in `describe()` (channel, action, longest message),
    which `GET /api/v1/drivers` passes through;
  - the `notify` action's contract.

  See PLUGINS.md.
- Plugin field types for the settings form:
  - `secret`: never shown back. Settings and `GET /api/v1/config` show a
    mask (`********`, plus the last four characters of a long value), and
    a mask sent back in a settings or config PUT keeps the real value.
  - `action`: a button that runs one of the plugin's actions with a value
    the user gives, and shows the result the plugin reports as `_action`
    in its settings.
  - `"readonly": true`: a value to show, such as credits left.

  Buttons and read-only values are left out of the Add Module form and
  never saved.

### Fixed

- The settings form sent any value that looked like a number (a phone
  number, a numeric name, a tap label or passcode) as a JSON number, and a
  value with a quote in it broke the request. Now only number and on/off
  rows are sent bare; everything else goes as JSON-escaped text.

## [0.9.0] - 2026-09-25

### Changed

- `moonflare-cli` and its MCP server now say "modules", as the TUI does.
  - **MCP tools:** `list_modules`, `query_module` and `module_history`;
    `status` is unchanged. The old names (`list_devices`, `query_device`,
    `device_history`) still work but are no longer listed.
  - **`--list`** takes `modules` (the default) or `drivers`. `devices`
    still works; any other word is now an error instead of listing
    modules.
  - Help text and messages say "module".
  - The REST paths (`/api/v1/devices`) are unchanged.
- The README says Modules where it still said Devices (the menu, and
  the System totals).

### Fixed

- The MCP server leaked memory on every `query_device` or
  `device_history` call made without `arguments`.

## [0.8.0] - 2026-09-24

### Added

- Magnum Energy inverters: `libmf_inverter_magnum.so` (kind `inverter`,
  driver `magnum`), a C port of pymagnum 2.0.8 by Charles Godwin
  (BSD-3-Clause, see `third_party/pymagnum/`). Each module is one passive
  RS-485 tap (an FTDI USB-RS485 adapter on a splitter), and it never writes
  to the bus.
  - **What it reads:** the inverter, plus the remote, router, AGS, BMK and
    PT-100 on the same network.
  - **Framing:** packets are framed by content, not timing, so daemon
    jitter or a stall loses nothing.
  - **The reading:** one JSON object under 4 KB with unit-suffixed keys, a
    `last_seen_s` per device, and `diag` counters (bytes, packets per device,
    bad packets, unknown bytes, resyncs, `0xFF` bytes). The `refresh` action
    resets the counters.
  - **Ports:** it opens only `/dev/serial/by-id/` paths (or finds the
    adapter by `usb.serial_id`) and refuses `/dev/ttyUSBn`. It won't open a
    port that another module has open or another reader has locked.
  - **Offline:** until the first inverter packet, and again after 5 s
    without one; the error says why.
  - **History:** 10 s, kept 60 days, graphing `dc_power_w`.

  Setup, every field with its pymagnum name, and the differences from
  pymagnum: `docs/magnum.md`.
- `mf_magnum_dump`, a read-only Magnum bus viewer like pymagnum's `magtest`.
  It records captures (`--capture`) and replays them, or pymagnum packet
  files (`--replay`).
- Two disabled example taps, `magnum-1` and `magnum-2`, in
  `etc/moonflared.json.example`.

## [0.7.0] - 2026-09-24

### Added

- Phantom modules: a built-in `phantom` driver for batteries, chargers
  and inverters, for a unit that is wired into the system but that
  moonflared cannot reach. A phantom shadows one or more real modules of
  its class (the `phantom.shadows` setting, a checklist in the TUI) and
  reports their average, recomputed every poll interval: numbers are
  averaged, per cell and per temperature sensor too; text and on/off
  values come from the first. Only shadowed modules that are active and
  online are used; with none, the phantom is offline and says why. It
  counts toward the system totals like any module of its class while
  active, records no history, and has no actions. The dashboard and the
  Modules menu mark it with `≈` at the right edge of its row (`~` without
  UTF-8), its pack view shows `phantom of: …`, and
  status rows carry `"phantom_of"`; `moonflare-cli` prints it and the MCP
  `status` tool describes it as an estimate.
- A `modules` field type for plugin settings: module UUIDs of the
  module's own kind, shown by name and edited as a checklist.

### Changed

- The never-populated `phantoms` list is gone from `/api/v1/status` and
  `moonflare-cli --status`; phantoms appear under their own class.
- A module that records no history no longer gets an empty history
  file.
- Graph Interval appears only for modules that record history.

### Fixed

- The settings dialog kept resetting Graph Interval to 30 when the
  module's settings arrived from the daemon.
- The Modules menu wrote past the end of its row-to-module table with
  more than 27 modules.
- The http_devices test daemon wrote its config to the real
  `/var/lib/moonflare/moonflared.json` path; it now uses a temp file.

### Requires

- libviper 7.8.0 (`vdk_has_utf8` and the right-edge row marker, for the
  `≈` / `~` phantom marker).

## [0.6.0] - 2026-09-24

### Changed

- The module settings dialog (**e** on the dashboard, or Add Module) now
  works like vwm's Settings. Every setting is one row of a list,
  `Label ........ [value]`, in a sunken frame, with Modify, Save and
  Close below (Modify, Add and Cancel when adding a module).
  - Enter or Modify edits the selected row in a popup that shows the
    setting's hint (Apply/Cancel); true/false settings get a two-item
    list, and Left/Right flips them in place.
  - Settings that cannot be changed (transport, cell count, UUID…) stay
    in the list, drawn in gray; Enter on one says it is read-only.
  - The title shows "(modified)" while there are unsaved changes. Save
    asks first ("Save module settings?"), then shows the daemon's answer:
    "Settings saved." or its error, with the dialog left open to fix it.
    Closing with unsaved changes asks whether to discard them.
  - Mouse: click a row to select it, click it again to modify it; the
    buttons and popup buttons are clickable.
- A settings save no longer gets lost when the connection has gone idle
  while the dialog was open: it is sent once the TUI reconnects, and
  reports "no reply from moonflared" after 10 s without an answer.

### Requires

- libviper 7.7.0 (`vk_listbox_set_item_colors`, for the gray rows).

## [0.5.1] - 2026-09-24

### Added

- `PLUGINS.md`, a guide to writing third-party plugins. It covers loading
  and naming, kinds and the reading keys the dashboard uses, the
  single-threaded event loop and what must not block, every field of the
  ops table, return codes, the configuration `open()` receives,
  `describe()`, settings and what gets saved, readings, history capture and
  pruning, actions, capabilities, probes, files and privileges, ABI
  compatibility, how to test against a private daemon, and a checklist.
  It ends with a complete example plugin (`service`/`loadavg`), compiled
  out of tree and verified against a live daemon.

## [0.5.0] - 2026-09-24

### Added

- Modules advertise history capture. A plugin's `describe()` may carry a
  `capture` block with the default and minimum interval, the reading
  fields to keep as columns (numeric or text, by dotted path), and the
  column to graph. `GET /api/v1/drivers` passes it through, and capturing
  drivers and modules list `history` in `caps`. All shipped plugins
  declare one: XD and JK batteries and the demo battery (10 s; pack_v,
  current_a, power_w, soc), Classic and the demo charger (10 s; pack_v,
  current_a, power_w), and weather.gov (600 s, minimum 60; temp_f,
  humidity_pct, wind_mph, conditions, icon, station).
- Every capturing module has a pruning policy, `retention_days` (whole
  days, 0 = keep forever). Plugins must declare a default in their capture
  block; one without it is rejected (the daemon logs a warning and the
  module records nothing). Users change it per module: Keep History in
  the settings form, or `retention_days` in a settings PUT. Each module
  prunes only its own file, hourly, at most 500 rows per main-loop pass.
  Every shipped plugin defaults to 60 days.
- Battery readings (XD, JK, demo) now include `power_w`.

### Changed

- History is one SQLite file per module,
  `/var/lib/moonflare/history/<uuid>.sqlite`, instead of one shared
  `history.sqlite`. Each keeps the whole reading plus the module's declared
  columns, and a `module` table with its identity and capture spec. Newly
  declared columns are added to existing files. Removing a module keeps its
  file and marks it retired.
- On first start the daemon migrates the old shared file into the
  per-module files, one transaction per module (an interrupted run
  resumes), and renames it `history.sqlite.migrated`. Old fixed-column
  values fill any declared column the stored JSON lacks, so battery
  `power_w` history carries over.
- The capture interval exists only for modules that capture. Settings
  omit `capture_interval_s` otherwise, and a PUT setting it non-zero on
  such a module is a 400. For capturing modules it must be 0 or at least
  the module's minimum. A module with no configured interval uses its
  plugin's default instead of a fixed 10 s.
- The graph shows whichever column the module declares (`graph`), not a
  column picked by kind.
- The settings and Add Module forms show Capture Interval only for
  modules that capture, seeded with the module's default. weather.gov no
  longer lists it among its own fields.

## [0.4.0] - 2026-09-24

### Added

- Two new module kinds, services and actuators. `GET /api/v1/status` has
  `services` and `actuators` arrays; their rows carry the module's whole
  reading as `data`. `moonflare-cli --status` lists both.
- The dashboard grid is now 3 × 2. The top row is Batteries, Chargers and
  an Info panel; the bottom row is Inverters, Actuators and Services. The
  Info panel shows the weather from the first active weather service:
  current temperature and conditions, humidity and wind, the next two
  forecast periods, and the station and time of the observation.
- A weather.gov service plugin (`libmf_service_weathergov.so`, kind
  `service`, driver `weathergov`). Add it with Devices → Add Module; the
  form asks for a ZIP code and a contact email, which weather.gov wants in
  the User-Agent. It fetches current conditions every 10 minutes
  (configurable, at least 5) and the forecast every 30, using libcurl
  without blocking the daemon. Its reading is provider-neutral, so another
  weather provider can feed the same panel. Building it needs
  `libcurl4-openssl-dev` and zlib; without libcurl it is skipped.
- ZIP codes are looked up offline in the Census Bureau's ZIP centroid
  table (`data/zcta.txt`, 33,791 ZIPs from the 2026 Gazetteer, installed to
  `share/moon-flare/zcta.txt`). Every `weather.zip_update_days` days
  (default 7, 0 = never) the plugin checks census.gov for the next year's
  table; when one appears it downloads, unzips and installs it in
  `/var/lib/moonflare/weathergov/`, which then takes priority over the
  shipped copy. Latitude and longitude remain as optional overrides.
- Weather icons in the Info panel. The weather plugin adds a neutral
  `icon` key (clear, partly_cloudy, mostly_cloudy, cloudy, wind, rain,
  showers, thunderstorm, snow, blizzard, sleet, freezing_rain, fog, haze,
  tornado, hurricane, hot, cold) and `is_day` to the current conditions and
  each forecast period, taken from weather.gov's own icon codes, or from the
  description when a station sends none. The dashboard shows ☀ or 🌙, ⛅,
  🌥, ☁, 🌦, 🌧, ⛈, 🌨, ❄, 🌫 and so on in a fixed 2-column slot, so the text
  lines up whether the terminal draws a symbol 1 or 2 columns wide. A
  forecast line that does not fit drops its words and keeps the icon.
  Needs libviper 7.6.3 for correct column layout of the symbols.
- The weather plugin has a Station setting (blank = nearest). It fetches
  the five nearest stations; an observation with no temperature or older
  than two hours, or a station that fails, is skipped for the next nearest
  one, and if none is good the last good reading stays up.
- Weather history: new weather modules capture their reading (including
  the forecast of the moment) to the history database every 600 s by
  default, matching the update interval, for later analysis.
- Weather settings can be changed after adding: select the module on the
  dashboard and press `e`. ZIP, station, table-check days, latitude and
  longitude, and contact take effect at once (a new place is looked up
  again) and are saved. The plugin interface already had get/put settings;
  the daemon now also saves plugin settings that a plugin's `describe()`
  declares, and `GET .../settings` returns the plugin's field labels as
  `_fields` so the dialog uses its wording.
- `e` on the dashboard opens the settings of the selected module, whatever
  its kind.
- A module offline now reports its `last_error` in
  `GET /api/v1/status`, and the Info panel shows a weather service's error
  (e.g. "ZIP 00000 not found") under "waiting for weather".
- Plugin-specific settings, such as the weather location, are now kept in
  the daemon's config and passed back to the plugin on start. Before this,
  any setting the daemon had no field for was dropped when the config was
  saved.

### Changed

- The Devices menu is now Modules.
- Choosing a module with no detail view (a weather service, say) from the
  Modules menu shows the dashboard with it selected, instead of opening an
  empty battery view.
- Readings of a module kind the dashboard has no place for are no longer
  shown as batteries.

## [0.3.0] - 2026-09-23

### Added

- Devices → Add Module and Remove Module in the TUI. Before this, both menu
  items did nothing. Add Module lists the drivers the daemon has loaded,
  then opens a form with that driver's fields and defaults (USB path and
  baud for XD, BLE address and adapter for JK, Modbus IP, port and unit for
  the Classic). If the daemon refuses, for example because the name is in
  use, the error shows in the form's title and the form stays open. Remove
  Module picks a module from a list and asks to confirm.
- Plugins describe their own Add Module form. The plugin interface has a
  new optional `describe()` entry that returns the fields to ask for
  (key, label, hint, type, default, required) and the bus.
  `GET /api/v1/drivers` passes it through as `bus` and `fields`, and the TUI
  builds the form from it, so neither the daemon nor the TUI needs to know
  a plugin's settings. Required fields are checked before the form is sent.
  The loader still accepts plugins built before `describe()` existed: it
  walks each plugin's table by that plugin's own entry size and treats the
  missing fields as absent.
- `POST /api/v1/devices` accepts dotted keys (`"usb.path": "/dev/ttyUSB0"`)
  as well as nested objects, and fills in the driver's `bus` if it is left
  out.

### Fixed

- The TUI crashed on quit (glibc abort from a double free) while tearing
  down the System bars. It now uses the same teardown as the pack view. The
  root cause was in libviper's `vk_progress_destroy()`, fixed in libviper
  7.6.2.
- Arrow keys now move between fields and buttons in the Add Module and
  device settings form, File → General, and the connection profile editor.
  Up/Down step through the fields and buttons, and Left/Right switch
  between the two buttons. Before this they did nothing unless the form
  could scroll. In the module pickers and the Connections list the
  selection did move, but the highlight was not redrawn.

### Changed

- `cmake --install` no longer installs the demo plugin (`libmf_demo.so`)
  into the production plugin directory, so "demo" no longer appears as a
  driver in Add Module. Configure with `-DMF_INSTALL_DEMO=ON` to install it.
  The example `moonflared.json` no longer has a `pack-demo` device.
- The daemon now owns its config at `/var/lib/moonflare/moonflared.json`
  (`$STATE_DIRECTORY/moonflared.json`). On first start it seeds that file
  from `/etc/moonflare/moonflared.json` (or `--config`) plus the older
  `settings.json` overlay; after that it reads only the state file. Every
  accepted change is saved to it, including adds, removes and
  `PUT /api/v1/config`, none of which were saved before. On a machine where
  `/etc/moonflare` is not writable, an added module used to vanish on
  restart and a removed one came back. `POST /config/save` and
  `/config/load` now use the state file. The state file is mode 0600
  because it holds JK app passcodes.


### Added

- A System panel at the top of the TUI dashboard, with three bars: total
  charger input, the combined battery state of charge (weighted by
  capacity), and total battery discharge. Each bar is green where filled and
  light gray where empty, with its reading shown inside.
- Devices can be marked active or inactive. Only active, online devices count
  toward the system totals. Press Space on the dashboard to toggle the selected
  device, or send `PUT /api/v1/devices/{id}/settings` with `{"active": false}`.
  The flag is saved in `moonflared.json` and the settings overlay, so it
  survives a restart.
- `GET /api/v1/status` has a new `system` object with the totals, and every
  device row (plus `GET /devices` and `GET /devices/{id}`) now has `active`.
  The totals use the same LFP voltage cross-check as the TUI, so the panel and
  the per-pack rows agree.
- A `system` section in `moonflared.json` sets the full-scale values of the
  Input and Discharge meters (`input_max_w`, default 3500 W;
  `discharge_max_w`, default 3000 W).
- Keyboard selection on the dashboard: arrow keys and Tab move a cursor between
  devices, Enter opens the selected device, and rows show `[x]` or `[ ]` for
  active or inactive.
- `moonflare-cli --status` prints the system totals and marks inactive
  devices. `--list` has an ACTIVE column. The MCP `status` and `list_devices`
  tools describe the new fields.
- The menu bar shows a clock and a moon-phase activity indicator.
- JK balance settings scroll, and show balance trigger and start-balance as
  read-only rows.

### Fixed

- A rewritten `moonflared.json` could not be loaded again. Any settings change
  saves the config, and on the next start every device read back an invented
  Modbus endpoint, so startup failed with "duplicate endpoint" and no devices
  came up. Re-loaded devices also had Classic auto-discovery switched on.
- A `capture_interval_s` change made through device settings was lost on
  restart.
- The dashboard hint line went black past its old width after the terminal was
  enlarged.
- Pack and Cells frames no longer overhang the window on first open.
- `usb_id` and the XD plugin no longer abort on hardened glibc builds when
  identifying a USB serial port.

### Changed

- The daemon, CLI, MCP and TUI version strings now come from the CMake project
  version. Before this they still said 0.1.0.
- C sources use Allman braces.

## [0.1.1] - 2026-09-21

### Changed

- The daemon now builds against protothread 2.0.0. The vendored `protothread.h`
  was updated to the header-only, freestanding v2 release, and the daemon's
  scheduler handle is now a `protothread_t` (the v2 name; `state_t` was removed).
  moon-flare uses only the documented protothread API, so no other call sites
  changed.

### Fixed

- The TUI HTTP client now times out a stalled in-flight request after 8 seconds
  and retries with backoff, instead of waiting indefinitely when the daemon
  accepts a connection but never answers.

## [0.1.0]

Initial daemon, REST API, plugins, CLI, and TUI.
