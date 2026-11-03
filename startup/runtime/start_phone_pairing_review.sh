#!/bin/sh
# Bounded user-assisted pairing session; no boot activation or flashing.
set -eu
seconds=${PBL_REVIEW_SECONDS:-1200}
trace=${PBL_REVIEW_TRACE:-0}
case "$trace" in 0|1) ;; *) exit 64;; esac
case "$seconds" in ''|*[!0-9]*) exit 64;; esac
[ "$seconds" = 0 ] || { [ "$seconds" -ge 30 ] && [ "$seconds" -le 86400 ]; } || exit 64
cd /opt/usr/home/owner/share/pebble
test ! -e enable-startup
stage=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
if [ ! -x "$stage/hci-ready-probe" ]; then
    printf 'Missing controller readiness probe; deploy it before launching\n' >&2
    exit 77
fi
# A disappearing UI marker means normal exit may still be restoring Samsung
# Bluetooth. Do not interrupt that cleanup with a second stop request.
if [ ! -s pebble-ui-active ]; then
    cleanup=0
    while systemctl is-active --quiet pebble-pairing-review.service; do
        cleanup=$((cleanup + 1))
        [ "$cleanup" -lt 60 ] || exit 74
        sleep 1
    done
fi
systemctl stop pebble-pairing-review.service || :
systemctl show pebble-pairing-review.service -p Result -p ExecMainStatus || :
if [ ! -e /sys/class/bluetooth/hci0 ]; then
    systemctl restart bluetooth-stack-up.service || :
    ready=0
    while [ ! -e /sys/class/bluetooth/hci0 ]; do
        ready=$((ready + 1))
        [ "$ready" -lt 30 ] || exit 70
        sleep 1
    done
fi
ready=0
until "$stage/hci-ready-probe" --ready; do
    ready=$((ready + 1))
    [ "$ready" -lt 30 ] || exit 75
    sleep 1
done
"$stage/hci-probe" || :
if [ -f pairing-review.log ]; then
    # Keep five generations: each relaunch used to overwrite the only crash evidence.
    for n in 4 3 2 1; do
        [ -f "$stage/pairing-review-previous.log.$n" ] &&
            mv "$stage/pairing-review-previous.log.$n" "$stage/pairing-review-previous.log.$((n + 1))"
    done
    [ -f "$stage/pairing-review-previous.log" ] &&
        mv "$stage/pairing-review-previous.log" "$stage/pairing-review-previous.log.1"
    cp pairing-review.log "$stage/pairing-review-previous.log"
fi
if [ -f pebbleos-flash.bin ]; then
    cp pebbleos-flash.bin "$stage/pebbleos-paired-state-backup.bin"
    chmod 0600 "$stage/pebbleos-paired-state-backup.bin"
fi
if [ -f "$stage/pebbleos-pairing-debug" ]; then
    cp "$stage/pebbleos-pairing-debug" pebbleos-pairing-debug.new
    chmod 0700 pebbleos-pairing-debug.new
    mv pebbleos-pairing-debug.new pebbleos-pairing-debug
fi
sed -e "s/-t 120/-c 12345 -t $seconds/" -e 's/^PBL_INPUT_GRAB=/PBL_OS_SWITCH=1 PBL_INPUT_GRAB=/; s/>pebble.log/>>pairing-review.log/; s/-S last-frame.ppm/-S pairing-review-frame.ppm/; s@^cd /opt/usr/home/owner/share/pebble@cd /opt/usr/home/owner/share/pebble\nrm -f pebble-ui-active@; /systemctl restart bluetooth-stack-down.service/i\
    systemctl stop bluez-start.service || :
' \
    run-pebble >run-pairing-review
if [ "$trace" = 1 ]; then
    sed 's/^PBL_OS_SWITCH=1 /PBL_OS_SWITCH=1 PBL_HCI_TRACE=1 PBL_INPUT_TRACE=1 /' \
        run-pairing-review >run-pairing-review.new
    mv run-pairing-review.new run-pairing-review
fi
if [ -x ./pebbleos-pairing-debug ]; then
    sed 's@\./pebbleos @./pebbleos-pairing-debug @' run-pairing-review >run-pairing-review.new
    mv run-pairing-review.new run-pairing-review
fi
# The adapter owns the display and wake policy now. The legacy interactive-only
# keep-awake holds extra wakeups and survives a supervised restart, so do not
# start it in a Pebble session at all.
sed '/^if \[ -x \.\/watch-keep-awake \]; then$/,/^fi$/d' \
    run-pairing-review >run-pairing-review.new
mv run-pairing-review.new run-pairing-review
# Keep duplicate stock UI workloads paused while Pebble runs. Terminating the
# stock watchface alone lets Samsung automatically launch it again.
if [ ! -f "$stage/watch-primary-ui-guard" ]; then exit 77; fi
cp "$stage/watch-primary-ui-guard" watch-primary-ui-guard
chmod 0700 watch-primary-ui-guard
awk -v seconds="$seconds" '
  /^awake=$/ { print; print "primary_guard="; next }
  /^restore\(\) \{/ {
    print;
    print "    if [ -n \"$primary_guard\" ]; then";
    print "        kill -TERM \"$primary_guard\" 2>/dev/null || :";
    print "        wait \"$primary_guard\" 2>/dev/null || :";
    print "    fi";
    print "    sh ./watch-primary-ui-guard --restore || :";
    print "    /usr/bin/app_launcher -s com.samsung.w-home || :";
    next;
  }
  { print }
' run-pairing-review >run-pairing-review.new
mv run-pairing-review.new run-pairing-review
# Hardware AOD reset can leave later normal wake broken. Do not run it here.
if grep -q 'watch-aod-kernel-review' run-pairing-review; then
    printf 'Refusing review runner containing experimental hardware AOD helper\n' >&2
    exit 72
fi
chmod 0700 run-pairing-review
sed 's@/run-pebble@/run-pairing-review@; s/TimeoutStopSec=35/TimeoutStopSec=90/; s/ bluez-start.service//' \
    pebble-startup-review.service >pebble-pairing-review.service
# Restart a crashed UI. The adapter is pulled in through a Wants dependency
# rather than ExecStartPost: start-post runs while the unit is still
# "activating", which the bridge script's is-active check rejects.
awk -v sec="$seconds" '
    /^\[Service\]$/ { print; print "Environment=PBL_REVIEW_SECONDS=" sec; next }
    /^Wants=display-manager.service$/ {
        print;
        print "Wants=pebble-device-bridge-review.service";
        print "StartLimitIntervalSec=600";
        print "StartLimitBurst=30";
        next;
    }
    /^Restart=no$/ {
        print "Restart=on-failure";
        print "RestartSec=2";
        next;
    }
    { print }
' pebble-pairing-review.service >pebble-pairing-review.service.new
mv pebble-pairing-review.service.new pebble-pairing-review.service
systemctl link --runtime /opt/usr/home/owner/share/pebble/pebble-pairing-review.service
systemctl daemon-reload
rm -f pebble-ui-active
: >pairing-review.log
: >device-bridge.log
systemctl start pebble-pairing-review.service
ready=0
while [ ! -s pebble-ui-active ]; do
    ready=$((ready + 1))
    if [ "$ready" -ge 45 ] || ! systemctl is-active --quiet pebble-pairing-review.service; then
        systemctl status pebble-pairing-review.service --no-pager || :
        tail -30 pairing-review.log
        systemctl stop pebble-pairing-review.service || :
        exit 71
    fi
    sleep 1
done
systemctl status pebble-pairing-review.service --no-pager
tail -45 pairing-review.log
if [ -f "$stage/start_watch_device_bridge_review.sh" ]; then
    # Refresh the adapter binary and its duration on every manual session
    # launch. On a supervised restart systemd starts it via Wants= instead.
    sh "$stage/start_watch_device_bridge_review.sh" || :
    if [ -f "$stage/start_watch_sensor_idle_review.sh" ]; then
        sh "$stage/start_watch_sensor_idle_review.sh" || :
    fi
    systemctl status pebble-device-bridge-review.service --no-pager || :
    cat device-bridge.log 2>/dev/null || :
fi
