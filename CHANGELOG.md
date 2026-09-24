# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
