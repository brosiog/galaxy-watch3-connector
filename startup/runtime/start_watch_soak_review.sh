#!/bin/sh
# Installs only a transient telemetry unit; no boot activation or flashing.
set -eu
seconds=${1:-${PBL_REVIEW_SECONDS:-86400}}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
stage=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
payload=/opt/usr/home/owner/share/pebble
test ! -e "$payload/enable-startup"
systemctl stop pebble-soak-review.service 2>/dev/null || :
cp "$stage/watch-soak" "$payload/watch-soak.new"
chmod 0700 "$payload/watch-soak.new"
mv "$payload/watch-soak.new" "$payload/watch-soak"
cat > "$payload/pebble-soak-review.service" <<UNIT
[Unit]
Description=PebbleOS extended-run telemetry
[Service]
Type=simple
SmackProcessLabel=User
ExecStart=/bin/sh -c 'exec /bin/sh /opt/usr/home/owner/share/pebble/watch-soak $seconds > /opt/usr/home/owner/share/pebble/soak-service.log 2>&1'
Restart=no
TimeoutStopSec=5
StandardOutput=null
StandardError=null
UNIT
systemctl link --runtime "$payload/pebble-soak-review.service"
systemctl daemon-reload
systemctl start pebble-soak-review.service
sleep 1
systemctl status pebble-soak-review.service --no-pager
head -2 "$payload/soak.log" 2>/dev/null || :
