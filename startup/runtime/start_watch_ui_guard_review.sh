#!/bin/sh
# Pause the duplicate stock watchface from a System-labelled unit: as User, scanning /proc is
# denied for every System process and each denial is audited (flash writes, suspend stalls).
set -eu
s=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
p=/opt/usr/home/owner/share/pebble
seconds=${PBL_REVIEW_SECONDS:-86400}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
test ! -e "$p/enable-startup"
cp "$s/watch-primary-ui-guard" "$p/watch-primary-ui-guard.new"
chmod 0700 "$p/watch-primary-ui-guard.new"
mv "$p/watch-primary-ui-guard.new" "$p/watch-primary-ui-guard"
cat > "$p/pebble-ui-guard-review.service" <<UNIT
[Unit]
Description=Pause the duplicate stock watchface while Pebble runs
BindsTo=pebble-pairing-review.service
After=pebble-pairing-review.service
StartLimitIntervalSec=600
StartLimitBurst=30
[Service]
Type=simple
SmackProcessLabel=System
ExecStartPre=-/bin/sh $p/watch-primary-ui-guard --restore
ExecStart=/bin/sh -c 'exec /bin/sh $p/watch-primary-ui-guard $seconds >> $p/primary-ui.log 2>&1'
ExecStopPost=/bin/sh $p/watch-primary-ui-guard --restore
Restart=on-failure
RestartSec=2
TimeoutStopSec=10
UNIT
systemctl link --runtime "$p/pebble-ui-guard-review.service" >/dev/null 2>&1 || :
systemctl daemon-reload
systemctl reset-failed pebble-ui-guard-review.service 2>/dev/null || :
systemctl start pebble-ui-guard-review.service
systemctl is-active pebble-ui-guard-review.service
