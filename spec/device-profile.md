# Device profile

A profile holds the values that differ between devices and do not belong in the adapter
protocol. The OS has a built-in profile for the Galaxy Watch3. A file replaces any value in it.

Pick the file with `-P file` on the OS command line, or `PBL_DEVICE_PROFILE`. Without either, the
built-in profile applies. If the file cannot be read, the OS says so and uses the built-in one.

## Format

One `key = value` per line. `#` starts a comment. Unknown keys and invalid values are reported
with their line number and skipped.

| Key | Value | Galaxy Watch3 |
| --- | --- | --- |
| `runtime_dir` | Directory for `adapter.sock`, `os.sock` and friends. Used when `-R`, `PBL_RUNTIME_DIR` and `XDG_RUNTIME_DIR` are all absent. | `/opt/usr/home/owner/share/pebble` |
| `data_dir` | Directory for persistent OS data. `PBL_DATA_DIR` overrides it. | `/opt/usr/home/owner/share/pebble` |
| `input.keys` | evdev device names for side buttons, comma-separated. A leading `*` matches a substring; anything else must match exactly. | `*power-keys, gpio_keys` |
| `input.touch` | evdev name of the touch panel. | `sec_touchscreen` |
| `input.bezel` | evdev name of the rotating bezel or crown. | `tizen_detent` |
| `foreground_reclaim_ms` | Up to four delays, in milliseconds after the display wakes, at which the OS checks that a vendor UI has not covered it and takes the foreground back if so. | `700, 2000` |

An empty list, such as `input.bezel =`, means the device has none.

## Example

```
# A watch with one button and a touch panel, no bezel, a vendor UI that never interferes.
runtime_dir = /run/pebble
input.keys = gpio-keys
input.touch = ts0
input.bezel =
foreground_reclaim_ms =
```

## Not in a profile

Display backend (Wayland socket or framebuffer path), resolution and shape come from the build
and from `PBL_WAYLAND_DISPLAY` and `PBL_FB_DEVICE`. Moving them into the profile is future work.
