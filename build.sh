#!/bin/sh
# Builds the ARM32 binaries for the Galaxy Watch3. Needs zig (https://ziglang.org).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
zig=${ZIG:-zig}
out=${OUT:-$here/out}
mkdir -p "$out"
for source in watch_device_bridge watch_sensor_idle watch_bridge_control conformance; do
  "$zig" cc -target arm-linux-gnueabi.2.24 -O2 -Wall -Wextra -Werror \
    -I"$here/include" -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
    "$here/adapter/$source.c" -ldl -lm -o "$out/$source"
  echo "built $out/$source"
done
