#!/bin/sh
set -eu
export XDG_RUNTIME_DIR=/run/user/5001
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/5001/bus
stage=/opt/usr/home/owner/share/tmp/pebble-startup-install
cleanup() {
    rm -f /run/user/5001/systemd/user/pebble-launcher-install.service
    rm -f "$stage/installer/libcoreclr.so" "$stage/install-worker.so" \
        "$stage/install-on-watch.sh"
    systemctl --user daemon-reload
}
trap cleanup 0
: > "$stage/owner-install.log"
systemctl --user link --runtime "$stage/pebble-launcher-install.service"
systemctl --user daemon-reload
result=0
systemctl --user start pebble-launcher-install.service || result=$?
cat "$stage/owner-install.log"
[ "$result" -eq 0 ]
grep -q '^PEBBLE_STARTUP_INSTALLED$' "$stage/owner-install.log"
systemctl --user daemon-reload
systemctl --user restart pebble-launcher.service
systemctl --user enable pebble-launcher.service
systemctl --user is-enabled pebble-launcher.service
printf 'PEBBLE_STARTUP_ENABLED\n'
