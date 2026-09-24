# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-09-23

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
