# Magnum inverters

`libmf_inverter_magnum.so` (kind `inverter`, driver `magnum`) reads a Magnum Energy inverter network from a passive RS-485 tap. Each module is one tap: an FTDI USB-RS485 adapter on a splitter. It never writes to the bus.

The protocol code is a C port of [pymagnum](https://github.com/CharlesGodwin/pymagnum) 2.0.8 by Charles Godwin (BSD-3-Clause; see [`third_party/pymagnum/`](../third_party/pymagnum/README.md)). Each ported file names the pymagnum file and function it comes from.

## Contents

- [How the bus is read](#how-the-bus-is-read)
- [Safety](#safety)
- [Setting up a tap](#setting-up-a-tap)
- [Online and offline](#online-and-offline)
- [The reading](#the-reading)
- [Where it differs from pymagnum](#where-it-differs-from-pymagnum)
- [History](#history)
- [mf_magnum_dump](#mf_magnum_dump)
- [Not done yet](#not-done-yet)

## How the bus is read

About every 100 ms the inverter sends a 21-byte packet. The remote (or the router) answers with 21 bytes whose last byte names the accessory it is polling, and at most one accessory replies (2 to 18 bytes, fixed by its first byte).

pymagnum ends a packet after 5 ms of silence. That timing doesn't survive a USB adapter's latency timer or an event loop shared with other modules, so this plugin frames by content instead:

- **Inverter packets anchor the cycle.** They are 21 bytes ending in `00`, with a known model, a plausible frequency, plausible temperatures and a plausible DC voltage. After the first one, the model and revision must match exactly.
- **Then the rest of the cycle.** The next 21 bytes are the remote packet (its type byte is checked), then an optional reply, then the next inverter packet.
- **Resync.** Anything that doesn't fit is skipped and counted, and the framer looks for the next inverter packet.
- **Timing only settles a cut-off packet.** If the bus goes quiet for 60 ms in the middle of a packet, those bytes count as bad.

A daemon stall changes nothing. The bytes wait in the kernel's tty buffer, which holds about 8 s of this bus, and decode in order once the daemon reads them. The PTY test checks this with a 2 s stall.

## Safety

- **Read-only.** The port is opened `O_RDONLY`, and nothing in the plugin writes to it.
- **Stable paths only.** `usb.path` must be a `/dev/serial/by-id/` link, or leave it empty and set `usb.serial_id`. The plugin and `mf_magnum_dump` both refuse `/dev/ttyUSBn` and `/dev/ttyACMn`, because the numbering changes between boots and the XD BMS owns `/dev/ttyUSB0`.
- **One reader per port.**
  - The plugin refuses a port that another module in this daemon already has open.
  - Once it opens a port, it takes an exclusive `flock` and sets `TIOCEXCL`, so a second reader can't open it: another module, `mf_magnum_dump`, or pymagnum.
  - It can't see ports held by other users' processes (`xd_bmsd` runs as root). The by-id path is the protection there.
- **Line settings are pymagnum's:** 19200 baud, 8N1, no flow control. The plugin also asks the FTDI driver for its 1 ms latency timer. Framing doesn't depend on it.

## Setting up a tap

1. **Find the adapter:** `ls -l /dev/serial/by-id/`. A Waveshare FT232RL appears as `usb-FTDI_FT232R_USB_UART_<serial>-if00-port0`.
2. **Watch the bus** with [`mf_magnum_dump`](#mf_magnum_dump) before adding a module. You should see INVERTER and REMOTE lines, and 0 bad packets.
3. **Add the module** with **Modules → Add Module → inverter magnum**, or copy one of the two disabled `magnum-*` entries from `etc/moonflared.json.example`. Replace the `XXXXXXX1` placeholder serials with yours.

| Setting | Default | Meaning |
| --- | --- | --- |
| `usb.path` | | The adapter's `/dev/serial/by-id/` link. |
| `usb.serial_id` | learned | The FTDI serial. It is learned from the path when empty. |
| `usb.auto_port` | true | If the path is missing, find the adapter by `usb.serial_id`. |
| `magnum.tap` | `""` | A label for where the tap is, up to 60 characters. It appears as `tap` in the reading. |
| `poll_interval_s` | 1 | How often the reading is rebuilt (at least 0.5 s). The bus itself is read continuously. |

`magnum.tap` and `poll_interval_s` can be changed in the settings form. The `usb.*` settings are fixed once the module exists (grayed out in the form); to move a tap, remove the module and add it again.

### Two taps, one per inverter

This site has two taps:

- **Tap 1:** a splitter on the ARTR's accessory port, shared with the MagWeb.
- **Tap 2:** a splitter on the second inverter's link at the ARTR.

The accessory port only carries one inverter's packets (pymagnum issue #5; the MagWeb shows the same), so each tap is its own module.

To see which inverter a tap hears, read `stackmode` and `stackmode_text`, which is byte 15 of the inverter packet: `Parallel stack - master` or `Parallel stack - slave`.

A tap follows the stack role of the first inverter packet it hears. It counts packets from any other role in `diag.other_inverter_packets`, and it switches to a new role after 5 s without packets from its own.

## Online and offline

The module is offline until its first valid inverter packet, and again after 5 s without one. Its error says why:

| Error | Meaning |
| --- | --- |
| `waiting for bus data on <port>` | The port was just opened. |
| `no data on the bus (<port>): tap or ARTR port silent` | Open for 5 s without one byte. Check the splitter and cable. An ARTR can take minutes to start talking on a newly connected port (issue #5). |
| `no data on the bus for N s (<port>)` | Bytes were arriving and stopped. |
| `no inverter yet: bus data but none from an inverter (N% 0xFF: check A/B wiring)` | Bytes arrive, but none parse as inverter packets. Mostly `0xFF` means A and B are swapped. It reads `no inverter packets:` once an inverter had been heard. |
| `adapter missing: …`, `adapter disconnected (<port>)` | Unplugged, or the by-id link is gone. The module retries every 2 s. |
| `<port> is already open (another module owns it)` | Another module has this port. The module retries every 10 s. |
| `<port> is in use by another reader` | Another process holds the port (`mf_magnum_dump`, say). |

## The reading

The reading is one JSON object, kept under 4 KB. It has:

- the inverter's values at the top level;
- a sub-object for each other device, once that device has been heard, with `last_seen_s`, the seconds since its last packet;
- `diag`, always.

Numbers are numbers, and units are key suffixes (`_v`, `_a`, `_w`, `_c`, `_h`, `_s`, `_hz`, `_pct`, `_kwh`, `_ah`). A key stays unsuffixed where neither pymagnum nor ME documents the unit. Clock times are `"HH:MM"` text, and values the device doesn't have are `null`.

### Inverter (top level)

| Key | pymagnum | Notes |
| --- | --- | --- |
| `tap` | | The module's `magnum.tap` label, or null. |
| `mode`, `mode_text` | `mode`, `mode_text` | `64`, `"INVERT"`. An unknown code reads `"??"`, as in pymagnum. |
| `fault`, `fault_text` | `fault`, `fault_text` | An unknown code reads `"Unknown"`. |
| `dc_voltage_v` | `vdc` | ME says it isn't accurate. |
| `dc_current_a` | `adc` | Whole amps. The sign is not yet confirmed. |
| `dc_power_w` | | Derived: `dc_voltage_v × dc_current_a`. |
| `ac_out_v`, `ac_out_a` | `VACout`, `AACout` | |
| `ac_in_v`, `ac_in_a` | `VACin`, `AACin` | `ac_in_v` is peak-to-peak, not RMS (per pymagnum). |
| `freq_hz` | `Hz` | |
| `invled`, `chgled` | `invled`, `invled_text`, `chgled`, `chgled_text` | true/false. |
| `bat_temp_c`, `tfmr_temp_c`, `fet_temp_c` | `bat`, `tfmr`, `fet` | |
| `model`, `model_text` | `model`, `model_text` | `115`, `"MS4448PAE"`. |
| `stackmode`, `stackmode_text` | `stackmode`, `stackmode_text` | Byte 15: which inverter this tap hears. |
| `revision` | `revision` | Firmware, as a number. |

### `remote`: the remote's settings

| Key | pymagnum | Notes |
| --- | --- | --- |
| `revision` | `revision` | |
| `search_w` | `searchwatts` | |
| `battery_type` | `battype` | A preset type, or 0 when absorb is set as a voltage. |
| `absorb_v` | `absorb` | 0 with a preset battery type. |
| `float_v` | `vsfloat` | |
| `eq_v` | `vEQ` | `absorb_v` plus an offset, so only the offset with a preset battery type (as in pymagnum). |
| `absorb_time_h` | `absorbtime` | |
| `charge_rate` | `chargeramps` | |
| `ac_input_a` | `ainput` | |
| `ac_cutout_v` | `vaccutout` | |
| `lbco_v` | `lbco` | No 24/48 V multiplier (as in pymagnum). |
| `parallel` | `parallel` | |
| `clock_time` | `remotetimehours`, `remotetimemins` | Null until a REMOTE_80 or REMOTE_A0 packet arrives. |
| `battery_size` | `batterysize` | Null until a REMOTE_80 packet arrives. |
| `battery_efficiency_pct` | `batteryefficiency` | Only once a BMK has answered. |

These appear only once an AGS has answered (pymagnum's `cleanup()`), each group once its remote packet has been seen:

| Key | pymagnum | Packet |
| --- | --- | --- |
| `gen_run_time_h` | `runtime` | A0 |
| `gen_start_temp_c` | `starttemp` | A0 |
| `gen_start_v` | `startvdc` | A0 |
| `quiet_time` | `quiettime` | A0 |
| `gen_start_time`, `gen_stop_time` | `begintime`, `stoptime` | A1 |
| `gen_stop_v` | `vdcstop` | A1 |
| `gen_volt_start_delay_s`, `gen_volt_stop_delay_s` | `voltstartdelay`, `voltstopdelay` | A1 |
| `gen_max_run_h` | `maxrun` | A1 |
| `gen_soc_start_pct`, `gen_soc_stop_pct` | `socstart`, `socstop` | A2 |
| `gen_amp_start_a`, `gen_amp_start_delay_s` | `ampstart`, `ampsstartdelay` | A2 |
| `gen_amp_stop_a`, `gen_amp_stop_delay_s` | `ampstop`, `ampsstopdelay` | A2 |
| `quiet_begin_time`, `quiet_end_time` | `quietbegintime`, `quietendtime` | A3 |
| `exercise_start_time` | `exercisestart` | A3 |
| `exercise_run_time_h` | `exerciseruntime` | A3 |
| `gen_topoff` | `topoff` | A3 |
| `gen_warmup_s`, `gen_cooldown_s` | `warmup`, `cool` | A4 |

pymagnum's `exercisedays` is never set, so it isn't reported.

### `router`

| Key | pymagnum | Notes |
| --- | --- | --- |
| `revision` | `revision` | For example `3.2`. |

### `ags`: the generator start module

| Key | pymagnum | Notes |
| --- | --- | --- |
| `revision` | `revision` | |
| `status`, `status_text` | `status`, `status_text` | |
| `running` | `running` | true/false. |
| `temp_c` | `temp` | Null when the AGS reports 105 °F or more (no sensor). |
| `run_time_h` | `runtime` | |
| `battery_voltage_v` | `vdc` | |
| `last_run`, `last_full_soc`, `total_run` | `gen_last_run`, `last_full_soc`, `gen_total_run` | From AGS_A2. pymagnum: "not reliably reported". |

### `bmk`: the battery monitor

| Key | pymagnum | Notes |
| --- | --- | --- |
| `revision` | `revision` | |
| `soc_pct` | `soc` | |
| `battery_voltage_v`, `battery_current_a` | `vdc`, `adc` | |
| `battery_voltage_min_v`, `battery_voltage_max_v` | `vmin`, `vmax` | Reset at power-up. |
| `net_ah` | `amph` | Usually negative. |
| `trip_ah` | `amphtrip` | Resettable. |
| `lifetime_ah` | `amphout` | |
| `fault`, `fault_text` | `Fault`, `Fault_Text` | |

### `pt100`: the PT-100 charge controller

Only address 0 is decoded, as in pymagnum.

| Key | pymagnum | Notes |
| --- | --- | --- |
| `address` | `address` | |
| `mode`, `mode_text` | `mode`, `mode_text` | |
| `regulation`, `regulation_text` | `regulation`, `regulation_text` | |
| `fault`, `fault_text` | `fault`, `fault_text` | An unknown code reads `"unknown"`, as in pymagnum. |
| `battery_voltage_v`, `battery_current_a` | `battery`, `battery_amps` | |
| `pv_voltage_v` | `pv_voltage` | |
| `charge_time` | `charge_time` | |
| `target_voltage_v` | `target_battery_voltage` | |
| `relay_on`, `alarm_on`, `fan_on`, `is_day` | `relay_state`, `alarm_state`, `fan_on`, `day` | true/false. |
| `bat_temp_c` | `battery_temperature` | Null when the sensor is shorted or open. |
| `inductor_temp_c`, `fet_temp_c` | `inductor_temperature`, `fet_temperature` | |

These appear once a PT_C2 packet has been seen:

| Key | pymagnum | Notes |
| --- | --- | --- |
| `revision` | `revision` | |
| `lifetime_kwh`, `resettable_kwh` | `lifetime_kwhrs`, `resettable_kwhrs` | |
| `ground_fault_current` | `ground_fault_current` | |
| `nominal_voltage_v` | `nominal_battery_voltage` | |
| `stacker_info` | `stacker_info` | |
| `dip_switches` | `dip_switches` | Text, such as `"00000101"`. |
| `output_current_rating_a` | `output_current_rating` | |
| `input_voltage_rating_v` | `input_voltage_rating` | |

### `acld`

Only `last_seen_s`: pymagnum recognizes ACLD_D1 packets but can't decode them.

### `diag`

| Key | Meaning |
| --- | --- |
| `bytes_read` | Bytes read from the port. |
| `packets` | Good packets per device: `inverter`, `remote`, `router`, `ags`, `bmk`, `pt100`, `acld`. |
| `bad_packets` | Packets that failed a check or were cut short. |
| `unknown_bytes` | Bytes skipped while looking for an inverter packet. |
| `resyncs` | How often the framer lost the cycle and had to look for it again. |
| `ff_bytes` | `0xFF` bytes. A large share means A and B are swapped. |
| `other_inverter_packets` | Packets from an inverter other than this tap's (see [Two taps](#two-taps-one-per-inverter)). |
| `last_inverter_s` | Seconds since this tap's last inverter packet. |
| `port` | The device node that is open. |
| `trimmed` | Present (true) if `pt100`, `bmk` or `remote` were left out to stay under 4 KB. |

To start the counters over: `curl -X POST http://<host>:5250/api/v1/devices/<uuid>/actions/refresh`.

## Where it differs from pymagnum

Representation, for consistency with the other modules:

- **Names.** Keys are unit-suffixed moon-flare names (the tables above), not pymagnum's.
- **Values.**
  - Revisions are numbers, not text.
  - `invled`, `chgled` and the PT-100 flags are true/false.
  - Clock times are `"HH:MM"`, not an `hhmm` number.
  - The remote's clock is one `clock_time` key.
- **Dropped keys.**
  - `invled_text` and `chgled_text` repeat the true/false values.
  - PT-100 `mode_hex` repeats `mode` and `regulation`.
  - PT-100 `model` is always 100 in pymagnum.

Fixes:

- **Router revision.** It is `3.2`, not rounded to `3`.
- **Exercise run time.** REMOTE_A3's run time goes to `exercise_run_time_h`. pymagnum writes it into `runtime`, overwriting the generator run time from REMOTE_A0, so that value flips between the two.
- **AGS temperature.** Readings of 105 °F or more are null. pymagnum passes the raw °F through, which doesn't fit a `_c` key.
- **Unknown inverter fault.** It reads `"Unknown"`. pymagnum keeps the previous fault's text.
- **Standby inverter packets.** They are kept. In pymagnum, a Standby inverter packet can be mistaken for a remote packet (a misplaced parenthesis). Framing by content never takes that path.
- **Voltage multiplier.** It comes from this tap's own inverter. In pymagnum it is shared by the whole process and set by whichever inverter packet came last.

Not ported:

- the old 16-byte protocol (INV_C and REMOTE_C);
- the experimental `--flip` option;
- the timing repairs, which content framing doesn't need: 22 → 21 and 17 → 16 trimming, and merging split packets.

**To check against real captures:**

- **The sign of `dc_current_a`.**
- **`lbco_v` on a 48 V system:** pymagnum applies no multiplier.
- **`eq_v` with a preset battery type.**

## History

A tap records its reading every 10 s (minimum 1 s) and keeps 60 days, like the other modules. The capture columns are:

- `dc_voltage_v`, `dc_current_a`, `dc_power_w`;
- `ac_out_v`, `ac_out_a`, `ac_in_v`, `ac_in_a`, `freq_hz`;
- `bat_temp_c`, `tfmr_temp_c`, `fet_temp_c`;
- `mode`, `fault`, `stackmode`, and `mode_text` as text.

The graph shows `dc_power_w`. The whole reading is stored with each sample too, so a column added later can be backfilled.

## mf_magnum_dump

A read-only bus viewer, the equivalent of pymagnum's `magtest`. It prints one line per packet, then a summary of the devices it detected:

```
mf_magnum_dump /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_<serial>-if00-port0
Length:21 INVERTER  =>4000020E0016780001003D11332473010005025800
Length:21 REMOTE_A0 =>00002808640A280000C89B840C14122014007300A0
Length: 6 AGS_A1    =>A102343A007F
...
  inverter: MS4448PAE, Parallel stack - master
  bytes 572, bad packets 0, unknown bytes 0, resyncs 0, 0xFF 0
```

Options:

| Option | Effect |
| --- | --- |
| `-n N` | Stop after N packets per port. The default is 50; 0 means no limit. |
| `-t S` | Stop after S seconds. The default is 30. |
| `--raw` | Also print each read as it arrives, with its time. |
| `--json` | Also print the reading the module would publish. |
| `--capture FILE` | Record every read as `<seconds> <hex>` (one port only). |
| `--replay FILE` | Play back a capture, or a pymagnum packet file (`Length:NN TYPE =>HEX` lines). |

Several ports can be watched at once; their lines are labelled. For example:

```
mf_magnum_dump -n 0 -t 60 --capture tap1.cap /dev/serial/by-id/usb-FTDI_…
mf_magnum_dump --json --replay tap1.cap
```

It can't open a port that a running module holds, and a module can't open a port while the dump holds it. To watch a tap that a module is using, remove the module or stop moonflared first. A capture replays with its original timing, so it can become a test case (`testdata/magnum/`).

## Not done yet

This release only adds the module. The daemon and the TUI treat inverters as they did before: listed on the dashboard, and not counted in the System totals. The next steps:

- **Dashboard row.** Show AC output power and `mode_text`. This needs an `ac_out_w` key (`ac_out_v × ac_out_a`, apparent power).
- **Inverter view.** A detail view in the TUI, like the pack and charger views: DC and AC values, temperatures, stack role, remote and AGS settings, and the `diag` counters with a Refresh action.
- **System totals.**
  - A Load value, the sum of the active inverters' AC output.
  - AC charging through the inverter, as a second input source next to the chargers.
  - No double counting: the battery current already includes the inverter's DC draw, so inverter DC power stays out of the discharge total.
- **DC current sign.** Confirm it from real captures before any total uses it.
- **Clients.** `moonflare-cli --status` and the MCP `status` tool should describe inverters.
- **Settings over REST.** `usb.*` changes in a settings PUT aren't saved to the config; this affects the XD module too.
