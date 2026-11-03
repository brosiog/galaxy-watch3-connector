#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
zig=${ZIG:-zig}
mkdir -p "$src/build"
export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-$src/build/zig-cache}
for source in bootstrap root-worker; do
    name=$source
    [ "$source" != bootstrap ] || name=libcoreclr
    "$zig" cc -target arm-linux-gnueabi.2.17 \
        -shared -fPIC -Os -Wall -Wextra -Werror "$src/$source.c" \
        -o "$src/build/$name.so"
done
stage=/opt/usr/home/owner/share/tmp/pebble-startup-install
"$zig" cc -target arm-linux-gnueabi.2.17 -shared -fPIC -Os -Wall -Wextra -Werror \
    "-DPEBBLE_WORKER_PATH=\"$stage/install-worker.so\"" \
    "$src/bootstrap.c" -o "$src/build/install-libcoreclr.so"
"$zig" cc -target arm-linux-gnueabi.2.17 -shared -fPIC -Os -Wall -Wextra -Werror \
    "-DPEBBLE_STARTUP_SCRIPT=\"$stage/install-on-watch.sh\"" \
    "$src/root-worker.c" -o "$src/build/install-worker.so"
sh -n "$src/start-pebble"
sh -n "$src/install-on-watch.sh"
sh -n "$src/setup-owner.sh"
sh -n "$src/install.sh"
