#!/bin/sh
# Installs only a transient listener; no boot activation or flashing.
set -eu
seconds=${PBL_REVIEW_SECONDS:-86400}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
stage=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
payload=/opt/usr/home/owner/share/pebble
test ! -e "$payload/enable-startup"
systemctl stop pebble-os-switch-review.service 2>/dev/null || :
cp "$stage/watch-os-switch" "$payload/watch-os-switch.new"
chmod 0700 "$payload/watch-os-switch.new"
mv "$payload/watch-os-switch.new" "$payload/watch-os-switch"
cat > "$payload/pebble-os-switch-review.service" <<UNIT
[Unit]
Description=Temporary three-second Samsung/Pebble switch
After=display-manager.service
StartLimitIntervalSec=600
StartLimitBurst=5
[Service]
Type=simple
SmackProcessLabel=User
Environment=PBL_REVIEW_SECONDS=$seconds
ExecStart=/bin/sh -c 'exec /opt/usr/home/owner/share/pebble/watch-os-switch $seconds >> /opt/usr/home/owner/share/pebble/os-switch.log 2>&1'
Restart=on-failure
RestartSec=5
TimeoutStopSec=5
StandardOutput=null
StandardError=null
UNIT
systemctl link --runtime "$payload/pebble-os-switch-review.service"
systemctl daemon-reload
: > "$payload/os-switch.log"
systemctl start pebble-os-switch-review.service
sleep 1
systemctl status pebble-os-switch-review.service --no-pager
cat "$payload/os-switch.log"
