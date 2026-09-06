# Moon Flare

Linux C11 daemon + libviper/VDK TUI for a mixed energy site: batteries, chargers, and (later) inverters.

Phase-1 design: [`docs/design.md`](docs/design.md). Skeleton (PR-1) is in-tree; REST, plugins, and the TUI land in later PRs.

Existing prototypes stay running until soak is done:

- `xd-battery` — TBD/XD BMS over RS485 (`xd_bmsd` :5252)
- `jkbms-tui` — JK BMS over BLE (`jkbmsd` :5253)

Moon Flare’s daemon listens on **:5250** (plain HTTP REST). The TUI is a VDK app (llama-chess menubar pattern). All views must render at **80×25**.

## Build

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Binaries

| Name | Role |
| --- | --- |
| `moonflared` | daemon (`--listen 127.0.0.1:5250 --foreground`) |
| `moonflare` | TUI client (`--help` stub until PR-7) |

Config: `/etc/moonflare/` (override `~/.config/moonflare/`). Examples in `etc/`. systemd unit: `contrib/moonflared.service` (`User=bryanc`; does not replace `xd_bmsd` / `jkbmsd`).
