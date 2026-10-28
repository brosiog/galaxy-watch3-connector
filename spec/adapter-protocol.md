# PebbleOS Linux device-adapter protocol, version 2

Status: implemented in the OS and in the Galaxy Watch3 adapter, 2026-10-28. It passes the host
test suites listed under "Tests". It has not run on a watch yet; the checks that need hardware are
listed under "Before release".

The wire contract is one header, `pebbleos/include/pbl/drivers/linux_adapter.h`. An adapter takes
that file and nothing else from the OS tree. This document explains the behavior around it.

## Roles

- **OS**: the PebbleOS native runtime. It owns the UI, services, apps, input decoding, drawing,
  and the display and backlight policy. It must keep running when no adapter is present.
- **Adapter**: a device-specific daemon. It owns vendor APIs (sensors, haptics, display power,
  power management) and vendor workarounds. It can be built for a different ABI than the OS; the
  Galaxy adapter is ARM32 because Tizen's userspace is ARMv7.
- **Adapter helpers**: extra processes that belong to the adapter, such as `watch_sensor_idle`.
  The OS never talks to them and writes nothing for them.

## Files

Everything lives in one runtime directory, chosen in this order:

1. `-R dir` on the OS command line (the adapter has no flag)
2. `PBL_RUNTIME_DIR`
3. `$XDG_RUNTIME_DIR/pebble`
4. the device profile's `runtime_dir`

| Path | Bound by | Purpose |
| --- | --- | --- |
| `adapter.sock` | adapter | Unix datagram socket. The OS sends here. |
| `os.sock` | OS | Unix datagram socket. The adapter sends here. |
| `fifo-policy` | adapter | Private to the adapter and its FIFO helper. |
| `sensor-trace.bin` | adapter | Raw sensor recording, written on a `TRACE` message. |
| `screen.ppm` | OS | Frame capture on `SIGUSR1`. A developer aid of the OS process; not protocol. |

Both sockets are mode 0600. Before replacing an existing socket file, a process checks that it is
a socket, owned by the same uid, and that `connect()` fails with `ECONNREFUSED`. A live endpoint is
never replaced, and a failed bind leaves a competing endpoint alone.

Socket paths must be shorter than 104 bytes.

Persistent OS data (`rtc-state.bin`) goes to a separate data directory, because runtime
directories may be tmpfs. The order is `PBL_RTC_STATE` for the file, else `PBL_DATA_DIR`, else the
profile's `data_dir`.

## Envelope

Every datagram starts with a 12-byte little-endian header:

| Field | Size | Meaning |
| --- | --- | --- |
| `magic` | u16 | `0x5042`, "PB" |
| `major` | u8 | 2 |
| `minor` | u8 | Additive revision within the major version |
| `type` | u16 | Message type |
| `length` | u16 | Total datagram bytes, header included |
| `seq` | u32 | Sender's counter |

A receiver drops a datagram with the wrong magic or major, an unknown type, a length below the
type's minimum, or a `length` larger than the bytes received. Bytes after `length` are ignored,
so a later minor version can append fields. Types `0x0000` to `0x7FFF` are core; `0x8000` and up
are debug or vendor and may be ignored. All reserved bytes are zero.

## Handshake

```
OS                                    adapter
 |                                       |   binds adapter.sock
 |                                       |-- ADAPTER_READY -->  (OS may not be bound yet)
 |-- HELLO {os_build, max_minor} ------->|   on OS start, and on every ADAPTER_READY
 |<-- CAPS {ack_seq = HELLO.seq, ...} ---|
 |<-- STATE ------------------------------|
 |-- ACCEL_CONFIG, HRM_CONFIG, BACKLIGHT, DISPLAY, IDLE (whatever the OS wants) -->
```

- Until `CAPS` arrives, the OS treats every capability as absent and ignores `SENSOR`, `GESTURE`,
  `STATE` and `FAULT`.
- The OS repeats `HELLO` at 0 s, 1 s, 3 s, 7 s and so on, doubling the gap up to 60 s.
- On `CAPS` the OS sends its cached configuration in full. It sends a message only for a
  capability the adapter declared.
- Failed nonblocking sends of cached configuration are retried after 100 ms. Haptic pulses and
  debug commands are never replayed this way. Accelerometer requests are clamped to the limits
  declared in `CAPS`.
- The adapter sends `STATE` on every change of display state, light level or acknowledgement,
  and at least every 10 s. After 180 s without any valid datagram the OS drops to "no adapter" and
  logs it once. `ADAPTER_READY` has the same effect at once, followed by a new `HELLO`.
- The OS does not heartbeat. The adapter learns that the OS is gone when a send to `os.sock` fails
  with `ECONNREFUSED` or `ENOENT`.

`CAPS` carries `ack_seq` (the `HELLO` it answers), a capability bitmask (`VIBE`, `BACKLIGHT`, `ALS`,
`ACCEL`, `HRM`, `STEPS`, `WRIST_GESTURE`, `DISPLAY_POWER`, `AMBIENT_LOWPOWER`, `SUSPEND`), the
accelerometer interval and sample limits, `vibe_max_duration_ms`, and 16-byte name and version
strings for bug reports.

## Messages

Payload layouts and `_Static_assert` sizes are in the header. Values are in Pebble units.

### OS to adapter

| Type | Payload | Behavior |
| --- | --- | --- |
| `HELLO` | `os_build[16]`, `max_minor` | Starts the handshake. |
| `VIBE` | `strength` 1-100, `duration_ms` up to 5000, `deadline_ms` | Strength or duration 0 stops. `deadline_ms` is a `CLOCK_BOOTTIME` time after which the command is stale; the adapter drops it. |
| `BACKLIGHT` | `level` 0-100 | Brightness only. Never changes display power. |
| `DISPLAY` | `mode` ON, OFF or AMBIENT_CLOCK; `reason` TIMEOUT, USER, PALM, GESTURE or APP | The OS is the authority for display mode. |
| `USER_ACTIVITY` | `source` BUTTON, BEZEL or TOUCH | Sent on every press, detent and touch-down. Lets the adapter light the panel at once and reset vendor idle timers. |
| `ACCEL_CONFIG` | `samples` 0-125, `interval_us` | `samples` 0 stops the stream. |
| `HRM_CONFIG` | `mode` OFF or BPM, `low_latency` | |
| `IDLE` | `busy`, `next_deadline_us` | See "Suspend". |
| `TRACE` | `enable`, `seconds` 1-300 | Debug range. Records `SENSOR` messages to `sensor-trace.bin`, capped at 1 MiB. |

### Adapter to OS

| Type | Payload | Behavior |
| --- | --- | --- |
| `ADAPTER_READY` | none | Sent once after the adapter binds. |
| `CAPS` | see above | Reply to `HELLO`. |
| `STATE` | `ack_seq`, `display_actual`, `lux`, `lux_valid` | `display_actual` is what the panel is doing now. `ack_seq` is the `seq` of the `DISPLAY` message the panel now reflects. |
| `SENSOR` | `type`, `count`, `interval_us`, `timestamp_us`, `values[125][3]` | Fixed size, 792 bytes. See below. |
| `GESTURE` | `kind`, `timestamp_us` | WRIST_UP, WRIST_DOWN, PALM_COVER, EXTERNAL_WAKE or EXTERNAL_OFF. The `EXTERNAL_*` kinds report the vendor stack lighting or blanking the panel. |
| `FAULT` | `component`, `state` | OK, RECOVERING or FAILED, for ACCEL, HRM, STEPS, DISPLAY, ALS or VIBE. Replaces silent recovery and marker files. |

`SENSOR` rows:

- ACCEL: `count` rows of `[x, y, z]` in milli-g, already in Pebble axis orientation, clamped to
  +-32767, evenly spaced at `interval_us`. Across a transport gap longer than 1.5 intervals the
  adapter flushes a short packet instead of inventing samples.
- HRM: one row, `[bpm, quality]`. `quality` is a Pebble `HRMQuality` value, with -1 for off-wrist.
  The adapter maps the vendor's accuracy; the OS clamps out-of-range values and reports a BPM only
  for 25 to 240 with quality above "worst".
- STEPS: one row, `[new steps since the last packet, elapsed seconds]`. New steps are at most
  30000, and the elapsed time keeps cadence between 60 and 180 steps per minute.

The Galaxy adapter advances `display_actual` and its acknowledgement after the vendor power
APIs accept the transition. A failed wake, lock, unlock or display-state call reports a DISPLAY
fault and retains the previous mode and acknowledgement for retry. Physical panel transition
latency still needs the on-watch checks below.

The OS validates `count` (1 to 125) and `interval_us` (1 ms to 1 s), and upsamples accelerometer
data when the delivered interval is an integer multiple (2 to 10) of the requested one.

## Display ownership

| Decision | Owner |
| --- | --- |
| Timeout length, honoring Pebble's backlight settings | OS (Pebble's light service) |
| Wake on button, bezel or touch | OS, which decodes input |
| Wake on wrist-up, off on wrist-down, palm cover | OS, from `GESTURE` |
| Turning the panel on or off, vendor locks, blanking, AOD/ALPM | adapter, executing `DISPLAY` |
| Light-sensor sampling while off, idle sensor rates, hub FIFO | adapter, privately |

How the OS maps its own events:

- The backlight driver turns a brightness of 0 into `DISPLAY` idle with reason TIMEOUT. A nonzero
  brightness becomes `BACKLIGHT` followed by `DISPLAY ON`. Idle means AMBIENT_CLOCK when the
  always-on clock preference is set and OFF otherwise. Changing the preference while idle resends
  `DISPLAY`.
- Input that wakes a sleeping display calls `light_enable_interaction()` (the event itself never
  reaches Pebble) and sends `USER_ACTIVITY` and, if the panel is not on, `DISPLAY ON`. A palm
  cover sends `DISPLAY` idle with reason PALM.
- WRIST_UP lights the display when the motion-backlight preference is on, and otherwise takes the
  panel back with `DISPLAY` idle. WRIST_DOWN always idles it. EXTERNAL_WAKE wakes like a button
  press. EXTERNAL_OFF needs no action: the next wake sees `display_actual` is not ON and resends
  `DISPLAY ON` even though the OS believed it was already on.

**Fast path.** Raise-to-wake latency is visible, and a round trip through the OS adds scheduling
delay. On WRIST_UP, `USER_ACTIVITY` and the vendor's LCDOn signal, the adapter lights the panel
before the OS answers and reports `display_actual = ON`. The OS then confirms with `DISPLAY ON`
or overrides with `DISPLAY` idle.

The adapter turns the panel off by itself in two cases only: the vendor's LCDOff signal (reported
as `EXTERNAL_OFF`), and 15 s after the OS has gone (nothing bound to `os.sock`), so a crashed OS
cannot keep the panel and its display lock on. The adapter's own 15 s inactivity timer no longer
exists; the timeout is Pebble's.

If no adapter is present, or it lacks `DISPLAY_POWER`, the OS stays interactive.

## Suspend

- The OS sends `IDLE {busy = 0, next_deadline_us}` before a sleep of a quarter second or longer,
  and `IDLE {busy = 1}` when it wakes from such a sleep. Shorter naps send nothing, so the OS
  stays marked busy through them. While busy it
  repeats the message every 2 s. `next_deadline_us` is absent (0) for sleeps over a day.
- The adapter, if it declared `SUSPEND`, takes a CPU power lock for `busy`. The lock expires after
  3 s on its own, so an OS that dies busy cannot hold the watch awake. For the deadline it arms a
  `CLOCK_BOOTTIME_ALARM` timer. It declares `SUSPEND` only if it could create that timer.
- After a waking input the OS holds its `EPOLLWAKEUP` source until `STATE` reports
  `display_actual = ON`, or 2 s pass. The 2 s is a timeout, not the mechanism.
- The OS does not touch `/sys/power` or wakeup counters.

## The FIFO helper

`watch_sensor_idle` puts the sensor hub into buffering mode while the screen is off. It reads
`fifo-policy`, a single line that the adapter rewrites on every `STATE`:

```
2 <monotonic_s> <screen_off> <accel_samples> <accel_interval_ms> <hrm_running> <os_alive> <frozen_events>
```

It buffers only when the line is under 20 s old, the screen is off, the accelerometer runs 125
samples at 40 to 250 ms, heart rate is off, and `os_alive` is 1. `os_alive` comes from whether the
adapter's last send to `os.sock` succeeded. A rise in `frozen_events` makes the helper restart
Samsung's sensor transport, at most once a minute. The helper no longer reads marker files or
scans `/proc`.

## Absent adapter

Commands return `-ENOTCONN` (or `-ENOSYS` for a capability the adapter lacks) and nothing is
sent. The ambient-light read returns `-EAGAIN`, or `-ENODEV` without the `ALS` capability. The
display stays interactive. The OS logs when an adapter appears and when it goes away.

## Tests

| What | Where | How to run |
| --- | --- | --- |
| Client against a mock adapter: handshake, backoff, capabilities, malformed input, restarts, silence, display flow, vibe deadlines, suspend, profile | `pebbleos/tests/linux_adapter/test_client.c` | `sh pebbleos/tests/linux_adapter/run.sh` |
| Conformance checker, proved against the mock | `tests/linux_adapter/conformance.c` | same script |
| Regressions found in review: bind race, idle deadline updates, spin cases, conformance limits | `tests/linux_adapter/test_*regressions.c`, `test_bind_race.c`, `test_conformance_checks.c` | same script |
| Sensor driver: interpolation, gaps, steps, HRM quality | `pebbleos/tests/fw/drivers/test_watch_sensor_linux.c` | `pbl test` |
| Galaxy adapter and FIFO helper logic, vendor libraries faked | `scripts/test_watch_sensor_bridge.c`, `test_watch_sensor_idle_policy.c`, `test_watch_haptic_deadline.c` | `sh scripts/run_adapter_tests.sh` |
| Display deadlines and fbdev/Wayland ambient behavior | `pebbleos/tests/linux_adapter/test_display_deadline.c` | `sh pebbleos/tests/linux_adapter/run_display_regressions.sh` |

The mock adapter (`mock_adapter_main.c`) also runs as a process with configurable capabilities,
display latency and a fast path, and can replay a recorded sensor trace, v1 or v2. Point the
conformance checker at any adapter with `conformance --dir RUNTIME_DIR [--full]`, with the OS
stopped.

## Before release

These need a watch and have not been run:

1. Raise-to-wake latency against the earlier measurements (`scripts/measure_wake_latency.sh`).
   The fast path and the OS-owned timeout both change this path.
2. A backlight timeout longer than 15 s, set in Pebble's settings, actually holds the display.
3. Wake by button, bezel and touch; palm cover; wrist-down; the Samsung charging screen.
4. Screen-off suspend with `IDLE` and the wake alarm, compared with the 4.6 %/h idle baseline.
5. The "Pulse" notification pattern, 20 repetitions, no long buzzes.
6. Heart-rate quality (still, moving, off-wrist) and step totals.
7. Frozen-accelerometer recovery through `fifo-policy`.
8. `conformance --full` against the real adapter, built for ARM32.
9. OS and adapter deployed together. Neither works with the other's previous version.

## What changed from the 2026-10-27 draft

- `VIBE` carries `deadline_ms`. The draft had plain duration, but the shipped v1 code had already
  added a deadline to stop an old pulse replaying after a stall.
- `CAPS` carries `ack_seq` in its payload, and `STATE` carries the `DISPLAY` acknowledgement the
  same way. The header has a single `seq`.
- `IDLE` is only sent for sleeps of a quarter second or more, and repeated while busy, so the
  adapter's lock can expire by itself.
- The OS-gone fallback (15 s) is new. Removing the adapter's inactivity timer needed it.
- Frame capture stayed in the OS as a developer aid and was not moved to a debug socket, because
  no adapter is involved. `TRACE` is in the debug range.
- The device profile has a format; see `device-profile.md`.
- `--runtime-dir` is `-R`, and `-P` names a profile file.

## Mapping from v1

| v1 | v2 |
| --- | --- |
| command 0, 1, 8 (vibe) | `VIBE` |
| command 2 with value > 0 | `BACKLIGHT` then `DISPLAY ON` |
| command 2 with value 0 | `DISPLAY` OFF or AMBIENT_CLOCK, reason TIMEOUT (or PALM) |
| command 3 | `USER_ACTIVITY` |
| command 4 | `ACCEL_CONFIG` |
| command 5 | `HRM_CONFIG` |
| command 6 | `TRACE` |
| command 7 | the mode of `DISPLAY`, and the OS's own clock preference |
| `sensor-data.sock`, packet type 1, 2, 4 | `SENSOR`, with types renumbered 1, 2, 3 |
| `READY` packet | `ADAPTER_READY` |
| `device-bridge-state` | `STATE` messages and `fifo-policy` |
| `pebble-ui-active`, `accel-frozen` | removed |

## Open questions

- Phone link and Bluetooth stay outside the adapter, since the OS uses the HCI user channel.
  Confirm that no future device needs an adapter-mediated radio.
- The wrist-gesture policy sits in the OS display driver. A second port would want it in a shared
  service.
- `SENSOR` stays fixed-size. At 25 Hz in batches of 125 the copy cost is small. A high-rate
  device would want a variable length.
