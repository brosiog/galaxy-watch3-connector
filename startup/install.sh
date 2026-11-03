#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sdb=${SDB:-sdb}
target=${PBL_TARGET:?Set PBL_TARGET to the watch IP:26101}
stage=/opt/usr/home/owner/share/tmp/pebble-startup-install
sh "$src/build.sh"
"$sdb" -s "$target" shell "mkdir -p $stage/installer"
for file in libcoreclr.so root-worker.so; do
    "$sdb" -s "$target" push "$src/build/$file" "$stage/$file"
done
"$sdb" -s "$target" push "$src/build/install-libcoreclr.so" "$stage/installer/libcoreclr.so"
"$sdb" -s "$target" push "$src/build/install-worker.so" "$stage/install-worker.so"
for file in start-pebble pebble-launcher.service install-on-watch.sh \
            pebble-launcher-install.service setup-owner.sh; do
    "$sdb" -s "$target" push "$src/$file" "$stage/$file"
done
result=$("$sdb" -s "$target" shell "sh $stage/setup-owner.sh")
printf '%s\n' "$result"
case "$result" in
    *PEBBLE_STARTUP_ENABLED*) ;;
    *) printf 'watch launcher setup failed\n' >&2; exit 1 ;;
esac
