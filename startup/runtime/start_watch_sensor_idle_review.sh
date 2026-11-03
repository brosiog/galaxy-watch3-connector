#!/bin/sh
set -eu
s=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
p=/opt/usr/home/owner/share/pebble
seconds=${PBL_REVIEW_SECONDS:-86400}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
test ! -e "$p/enable-startup"
grep -q 'Model=SM-R850;' /etc/info.ini
grep -q 'Build=R850XXU1DWK2;' /etc/info.ini
systemctl stop pebble-sensor-idle-review.service 2>/dev/null || :
for name in watch-sensor-idle watch-sensor-hub-state watch-wristup-rearm; do
  cp "$s/$name" "$p/$name.new"
  chmod 0700 "$p/$name.new"
  mv "$p/$name.new" "$p/$name"
done
cat > "$p/pebble-sensor-idle-review.service" <<UNIT
[Unit]
Description=Pebble sensor hub FIFO idle policy
BindsTo=pebble-pairing-review.service pebble-device-bridge-review.service
PartOf=pebble-device-bridge-review.service
After=pebble-pairing-review.service pebble-device-bridge-review.service
StartLimitIntervalSec=600
StartLimitBurst=30
[Service]
Type=simple
SmackProcessLabel=System
ExecStart=/bin/sh -c 'exec $p/watch-sensor-idle $seconds >> $p/sensor-idle.log 2>&1'
ExecStopPost=$p/watch-sensor-hub-state resume
Restart=on-failure
RestartSec=2
TimeoutStopSec=10
UNIT
systemctl link --runtime "$p/pebble-sensor-idle-review.service"
systemctl daemon-reload
systemctl reset-failed pebble-sensor-idle-review.service || :
systemctl start pebble-sensor-idle-review.service
systemctl is-active pebble-sensor-idle-review.service
