# Moon Flare

Linux C11 daemon + libviper/VDK TUI for a mixed energy site: batteries, chargers, and (later) inverters.

Phase-1 design: [`docs/design.md`](docs/design.md).

Existing prototypes stay running until soak is done:

- `xd-battery` — TBD/XD BMS over RS485 (`xd_bmsd` :5252)
- `jkbms-tui` — JK BMS over BLE (`jkbmsd` :5253)

Moon Flare’s daemon listens on **:5250** (plain HTTP REST). It does not bind 5252 or 5253 and does not replace those units. The TUI is a VDK app (llama-chess menubar pattern). All views must render at **80×25**.

## Build

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Binaries

| Name | Role |
| --- | --- |
| `moonflared` | daemon (`--listen 127.0.0.1:5250 --foreground`) |
| `moonflare` | TUI (`--connect 172.16.0.65:5250`; F10 menubar) |
| `mf_gatt` | BlueZ helper (`/usr/local/libexec/mf_gatt`; never `$PATH`) |

Plugins (`libmf_demo.so`, `libmf_charger_classic.so`, `libmf_battery_xd.so`, `libmf_battery_jk.so`) live in one `--plugin-dir` (default `/usr/local/lib/moon-flare`). The build tree splits them under `build/*_plugins/` so tests load one driver at a time.

## Install

`cmake --install build` puts binaries under `$prefix` (default `/usr/local`), plugins in `/usr/local/lib/moon-flare`, `mf_gatt` in `/usr/local/libexec`, and the unit + examples in `/usr/local/share/moon-flare`. **It does not install or enable a systemd unit.** Copy those yourself:

```
sudo mkdir -p /etc/moonflare
sudo cp /usr/local/share/moon-flare/moonflared.json.example /etc/moonflare/moonflared.json
sudo cp /usr/local/share/moon-flare/moonflare.json.example /etc/moonflare/moonflare.json
sudo cp /usr/local/share/moon-flare/moonflared.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Do not `systemctl enable --now moonflared` until the soak stage below matches the live site. The example config enables **demo + Classic only**; `pack-xd` and `pack-jk` are `"enabled": false` so an accidental start cannot steal ttyUSB0 or the JK MAC from `xd_bmsd` / `jkbmsd`.

Config search (daemon `moonflared.json`, TUI `moonflare.json`): `--config`, then `~/.config/moonflare/`, then `/etc/moonflare/`. systemd `StateDirectory=moonflare` is `/var/lib/moonflare` (history + endpoint overlay).

## Soak (batteryman)

Order from the design Rollout. Do not skip to JK.

| Stage | What runs | Prototypes | Notes |
| --- | --- | --- | --- |
| 1. elite-tux, no hardware | demo battery (+ optional demo charger) | untouched | Combine `build/demo_plugins/*.so` (and Classic if wanted) into one `--plugin-dir`. |
| 2. batteryman, Classic | `libmf_charger_classic.so` + demo | **keep** `xd_bmsd` and `jkbmsd` | Classic is unused by the prototypes. One Modbus TCP client — drop Local App / HA on :502 first. Confirm register 4209 and watts against `pyclassic.py`. |
| 3. XD USB | `libmf_battery_xd.so` | stop using ttyUSB0 in `xd_bmsd`, **or** leave `pack-xd` disabled | USB can be opened by two processes — do not. Prefer a recorded PTY first, then a maintenance window. |
| 4. JK BLE | `libmf_battery_jk.so` + `mf_gatt` | `systemctl stop jkbmsd` **only at this cutover** | BLE is exclusive per MAC (`28:D4:1E:A7:23:39`). Last. |
| 5. Daily driver | `moonflare --connect 172.16.0.65:5250` | remain installed for rollback | Operator retires prototypes in a later phase. |

Health: `curl -sS http://127.0.0.1:5250/api/v1/health`.

systemd unit: `contrib/moonflared.service` (`User=bryanc`, `SupplementaryGroups=dialout bluetooth`). **Additive.** No `Conflicts=` on `xd_bmsd` / `jkbmsd`. Never `systemctl disable` those from this repo.

### Rollback

- **Demo / Classic-only:** `systemctl stop moonflared`. Prototypes were never stopped.
- **After XD USB cutover:** restore `xd_bmsd --battery ttyUSB0:/dev/ttyUSB0:9600:1` (`jkbms-tui/contrib/xd_bmsd.service`) and keep `pack-xd` `"enabled": false` (or omit it).
- **After JK BLE cutover:** `systemctl enable --now jkbmsd` so it reclaims the MAC; set `pack-jk` `"enabled": false`. Both daemons cannot own the pack.

Config write-back does not touch `xd_bmsd` argv.
