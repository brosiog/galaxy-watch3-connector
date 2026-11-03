#!/bin/sh
set -eu
seconds=${PBL_REVIEW_SECONDS:-1200}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
stage=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
payload=/opt/usr/home/owner/share/pebble
test ! -e "$payload/enable-startup"
systemctl is-active --quiet pebble-pairing-review.service
systemctl stop pebble-device-bridge-review.service 2>/dev/null || :
cp "$stage/watch-device-bridge" "$payload/watch-device-bridge.new"
chmod 0700 "$payload/watch-device-bridge.new"
mv "$payload/watch-device-bridge.new" "$payload/watch-device-bridge"
cat >"$payload/pebble-device-bridge-review.service" <<'UNIT'
[Unit]
Description=PebbleOS temporary Samsung device adapter
Wants=pebble-sensor-idle-review.service pebble-ui-guard-review.service
BindsTo=pebble-pairing-review.service
After=pebble-pairing-review.service
StartLimitIntervalSec=600
StartLimitBurst=30

[Service]
Type=simple
SmackProcessLabel=User
ExecStart=/bin/sh -c 'exec /opt/usr/home/owner/share/pebble/watch-device-bridge 1200 >> /opt/usr/home/owner/share/pebble/device-bridge.log 2>&1'
Restart=on-failure
RestartSec=2
TimeoutStopSec=10
StandardOutput=null
StandardError=null
UNIT
sed "s/watch-device-bridge 1200/watch-device-bridge $seconds/" "$payload/pebble-device-bridge-review.service" > "$payload/pebble-device-bridge-review.service.new"
mv "$payload/pebble-device-bridge-review.service.new" "$payload/pebble-device-bridge-review.service"
systemctl link --runtime "$payload/pebble-device-bridge-review.service"
systemctl daemon-reload
systemctl start pebble-device-bridge-review.service
sleep 2
# Release the older interactive-only display keeper after the adapter owns wake.
tasks=/sys/fs/cgroup/systemd/system.slice/pebble-pairing-review.service/cgroup.procs
if [ -r "$tasks" ]; then
  for pid in $(cat "$tasks"); do
    executable=$(readlink "/proc/$pid/exe" 2>/dev/null || :)
    if [ "$executable" = "$payload/watch-keep-awake" ]; then
      kill -TERM "$pid"
    fi
  done
fi
systemctl status pebble-device-bridge-review.service --no-pager
cat "$payload/device-bridge.log"
