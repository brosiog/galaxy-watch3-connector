#!/bin/sh
# Extended wear session: PebbleOS plus adapter, re-launch listener and telemetry.
# Runtime only; no boot activation or flashing.
set -eu
seconds=${PBL_WEAR_SECONDS:-86400}
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
listener_seconds=0
if [ "$seconds" != 0 ]; then
    listener_seconds=$((seconds + 3600))
    [ "$listener_seconds" -le 86400 ] || listener_seconds=86400
fi
stage=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
export PBL_REVIEW_SECONDS="$seconds"
systemctl stop pebble-os-switch-review.service 2>/dev/null || :
restore_listener() {
    PBL_REVIEW_SECONDS="$listener_seconds" \
        sh "$stage/start_watch_os_switch_review.sh" || :
}
trap restore_listener 0
sh "$stage/start_watch_soak_review.sh" "$seconds" || :
sh "$stage/start_phone_pairing_review.sh"
