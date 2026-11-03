#!/bin/sh
# Builds and runs the Galaxy Watch3 adapter's unit tests on the host. They exercise the real
# adapter and FIFO-helper sources with the vendor libraries faked. On macOS a small shim stands in
# for the Linux timer API.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
# The shared protocol header: next to this directory in the connector repo, inside the OS tree here.
inc=$root/include
[ -d "$inc" ] || inc=$root/pebbleos/include
out=${BUILD_DIR:-$(mktemp -d)}
cc=${CC:-cc}

run() {
  name=$1
  shift
  if [ "$(uname)" = Linux ]; then
    "$cc" -std=gnu11 -Wall -Wextra -Werror -I"$inc" -I"$here" \
      "$here/$name.c" "$@" -ldl -lm -o "$out/$name"
  else
    "$cc" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -D_DARWIN_C_SOURCE \
      -DCLOCK_BOOTTIME=CLOCK_MONOTONIC -DCLOCK_BOOTTIME_ALARM=CLOCK_MONOTONIC \
      -I"$here/host-shim" -I"$inc" -I"$here" \
      "$here/$name.c" "$here/host-shim/timerfd.c" "$@" -lm -o "$out/$name"
  fi
  "$out/$name"
}

run test_watch_haptic_deadline
run test_watch_session
run test_watch_sensor_idle_policy
run test_watch_sensor_bridge
"$cc" -std=gnu11 -Wall -Wextra -Werror -I"$inc" \
  "$here/watch_bridge_control.c" -o "$out/watch_bridge_control"
sh "$here/test_watch_bridge_control.sh" "$out/watch_bridge_control"
echo ADAPTER_UNIT_TESTS_OK
