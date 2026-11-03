#!/bin/sh
set -u
p=/opt/usr/home/owner/share/pebble
s=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
seconds=${PBL_SESSION_SECONDS:-0}
cat /proc/uptime
test ! -e "$p/enable-startup" || exit 9
PBL_WEAR_SECONDS="$seconds" sh "$s/start_watch_wear_review.sh" > "$s/relaunch-wear.log" 2>&1; echo "wear_rc=$?"
tail -5 "$s/relaunch-wear.log"
PBL_REVIEW_SECONDS="$seconds" sh "$s/start_watch_device_bridge_review.sh" > "$s/relaunch-bridge.log" 2>&1; echo "bridge_rc=$?"
PBL_REVIEW_SECONDS="$seconds" sh "$s/start_watch_sensor_idle_review.sh" 2>&1 | tail -1
PBL_REVIEW_SECONDS="$seconds" sh "$s/start_watch_ui_guard_review.sh" 2>&1 | tail -1
sleep 4
systemctl is-active pebble-pairing-review.service pebble-device-bridge-review.service pebble-sensor-idle-review.service pebble-ui-guard-review.service pebble-os-switch-review.service pebble-soak-review.service
grep -E 'StartLimitBurst|RestartSec' $p/pebble-pairing-review.service
grep -c watch-primary-ui-guard $p/run-pairing-review
sha256sum $p/pebbleos-pairing-debug
"$s/watch-console-probe" 'app launch 1' >/dev/null 2>&1
"$s/watch-console-probe" 'app active' 'time show' 'bt status' 2>&1 | grep -E 'app name|Local|Region|Connected' || :
echo RELAUNCH_DONE
