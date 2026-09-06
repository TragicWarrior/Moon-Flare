# Moon Flare

Linux C11 daemon + libviper/VDK TUI for a mixed energy site: batteries, chargers, and (later) inverters.

This repository is new. The design for phase 1 is in [`docs/design.md`](docs/design.md). **No application code yet.**

Existing prototypes stay running until soak is done:

- `xd-battery` — TBD/XD BMS over RS485 (`xd_bmsd` :5252)
- `jkbms-tui` — JK BMS over BLE (`jkbmsd` :5253)

Moon Flare’s daemon listens on **:5250** (plain HTTP REST). The TUI is a VDK app (llama-chess menubar pattern). All views must render at **80×25**.

## Binaries (planned)

| Name | Role |
| --- | --- |
| `moonflared` | daemon, plugins, SQLite capture |
| `moonflare` | TUI client |

Config: `/etc/moonflare/` (override `~/.config/moonflare/`).
