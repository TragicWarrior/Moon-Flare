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

Plugins (`libmf_charger_classic.so`, `libmf_battery_xd.so`, `libmf_battery_jk.so`) live in one `--plugin-dir` (default `/usr/local/lib/moon-flare`). The build tree splits them under `build/*_plugins/` so tests load one driver at a time. The demo plugin (`libmf_demo.so`, a fake battery and charger for development) is built but not installed unless you configure with `-DMF_INSTALL_DEMO=ON`; to use it without installing, point `--plugin-dir` at `build/demo_plugins`.

The weather.gov service plugin (`libmf_service_weathergov.so`) needs libcurl: install `libcurl4-openssl-dev` to build it (it is skipped otherwise), and the target needs the `libcurl4` runtime. Add it with Devices → Add Module; it asks for a ZIP code and a contact email for the NWS User-Agent, and feeds the dashboard's Info panel. ZIP codes are looked up offline in `data/zcta.txt` (Census Bureau ZIP centroids, installed to `share/moon-flare/zcta.txt`); the plugin checks census.gov for a newer yearly table every `weather.zip_update_days` days (default 7) and keeps updates in `/var/lib/moonflare/weathergov/`.

## Install

`cmake --install build` puts binaries under `$prefix` (default `/usr/local`), plugins in `/usr/local/lib/moon-flare`, `mf_gatt` in `/usr/local/libexec`, and the unit + examples in `/usr/local/share/moon-flare`. **It does not install or enable a systemd unit.** Copy those yourself:

```
sudo mkdir -p /etc/moonflare
sudo cp /usr/local/share/moon-flare/moonflared.json.example /etc/moonflare/moonflared.json
sudo cp /usr/local/share/moon-flare/moonflare.json.example /etc/moonflare/moonflare.json
sudo cp /usr/local/share/moon-flare/moonflared.service /etc/systemd/system/
sudo systemctl daemon-reload
```

Do not `systemctl enable --now moonflared` until the soak stage below matches the live site. The example config enables **Classic only**; `pack-xd` and `pack-jk` are `"enabled": false` so an accidental start cannot steal ttyUSB0 or the JK MAC from `xd_bmsd` / `jkbmsd`.

Config search (daemon `moonflared.json`, TUI `moonflare.json`): `--config`, then `~/.config/moonflare/`, then `/etc/moonflare/`. systemd `StateDirectory=moonflare` is `/var/lib/moonflare` (history + the daemon's working config).

### Where the daemon keeps its config

The daemon owns its working config at `/var/lib/moonflare/moonflared.json` (`$STATE_DIRECTORY/moonflared.json` under systemd). On its first start it seeds that file from `/etc/moonflare/moonflared.json` (or `--config`), plus any older `settings.json` overlay. From then on it reads only the state file, and saves the whole thing whenever something changes: device settings, the active flag, and modules added or removed with Devices → Add Module / Remove Module in the TUI.

`/etc/moonflare/moonflared.json` is only the first-run seed after that. To apply an edit to it, stop the daemon, move `/var/lib/moonflare/moonflared.json` aside, and start it again; it reseeds from `/etc`. The state file is mode 0600 because it holds JK app passcodes. If it ever fails to parse, the daemon renames it to `moonflared.json.bad` and reseeds.

## System totals

The dashboard's System panel (and `moonflare-cli --status`, and the MCP `status` tool) shows site-wide Input, Capacity and Discharge. Only devices marked **active** count. A device that reports readings but isn't wired into the system (a pack on the bench, say) can stay enabled but inactive: select it on the dashboard and press **Space**, or `PUT /api/v1/devices/{id}/settings` with `{"active": false}`. The flag persists across restarts. The meters' full scale comes from `moonflared.json`:

```json
"system": { "input_max_w": 3500, "discharge_max_w": 3000 }
```

## History

Each module that supports it records its readings in its own SQLite file, `/var/lib/moonflare/history/<module-uuid>.sqlite`, so modules never share a table. A plugin advertises capture in its `describe()` (`"capture"`: default and minimum interval, which reading fields become columns, which column to graph); a module whose plugin advertises none records nothing and has no Capture Interval setting. `GET /api/v1/drivers` passes the spec through, and capturing drivers and modules list `history` in their `caps`.

Each file has a `module` key/value table (uuid, name, kind, driver, created and retired times, the capture spec) and `samples(id, ts, online, reading, <declared columns>…)`. `reading` is the module's whole JSON reading, so a column a newer plugin declares can be backfilled with `json_extract`. New columns are added to an existing file automatically. Removing a module keeps its file and marks it retired.

Every capturing module must also declare a pruning policy, `retention_days`: samples older than that are deleted from its file (0 = keep forever). A capture spec without one is rejected, and the daemon logs a warning at load. Each module prunes only its own file, about once an hour, in batches of at most 500 rows per main-loop pass, so a large backlog never stalls polling or REST. SQLite reuses the freed pages, so a file stops growing once it reaches its window; it does not shrink on disk (run `VACUUM` on a stopped daemon's file to reclaim space).

Defaults:

| Module | Capture | Keep | Graph |
| --- | --- | --- | --- |
| Batteries (XD, JK) | 10 s | 60 days | `soc` |
| Chargers (Classic) | 10 s | 60 days | `power_w` |
| weather.gov | 600 s (at least 60) | 60 days | `temp_f`, plus humidity, wind, conditions, icon, station |

Change them per module in the settings form (Capture Interval, Keep History), or `PUT /api/v1/devices/{id}/settings` with `{"capture_interval_s": N}` (0 = off) or `{"retention_days": N}` (whole days, 0 = forever).

**Upgrading from 0.4 or earlier:** on its first start the daemon copies the old shared `/var/lib/moonflare/history.sqlite` into the per-module files, one transaction per module (so an interrupted run resumes), then renames it `history.sqlite.migrated`. Removed modules get a file too, marked retired. Delete `history.sqlite.migrated` once you are happy with the result. `history.dir` and `history.path` in `moonflared.json` move the per-module directory and the old file.

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
