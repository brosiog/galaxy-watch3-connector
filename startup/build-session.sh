#!/bin/sh
# Build the helpers once; the installed OS executable already supports -t 0.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
pebble=${PEBBLEOS_SOURCE:?Set PEBBLEOS_SOURCE to the matching PebbleOS source tree}
headers=$repo/adapter
[ -d "$headers" ] || headers=$here/../../scripts
sources=${SESSION_SOURCE_DIR:-$headers}
zig=${ZIG:-zig}
out=${SESSION_OUT:-$here/build/session}
mkdir -p "$out"
"$zig" cc -target arm-linux-gnueabi.2.24 -O2 -Wall -Wextra -Werror \
    -I"$pebble/include" -I"$repo/include" -I"$headers" \
    -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
    "$sources/watch_device_bridge.c" -ldl -lm -o "$out/watch-device-bridge"
"$zig" cc -target aarch64-linux-musl -static -O2 -Wall -Wextra -Werror \
    -I"$pebble/include" -I"$repo/include" -I"$headers" \
    "$sources/watch_sensor_idle.c" -o "$out/watch-sensor-idle"
for spec in watch_os_switch:watch-os-switch test_watch_switch_state:test-watch-switch-state; do
    source=${spec%%:*}
    name=${spec#*:}
    "$zig" cc -target aarch64-linux-musl -static -O2 -Wall -Wextra -Werror \
        -I"$pebble/include" -I"$pebble/soc/posix/include" -I"$headers" \
        "-DPEBBLE_INPUT_SOURCE=\"$pebble/drivers/button/linux/input_linux_bottom.c\"" \
        "$here/runtime/$source.c" "$pebble/drivers/linux_adapter/linux_profile.c" \
        -o "$out/$name"
done
printf 'SESSION_BINARIES=%s\n' "$out"
