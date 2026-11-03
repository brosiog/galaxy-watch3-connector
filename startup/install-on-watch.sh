#!/bin/sh
# Run by the temporary setup bootstrap through the owner's service manager.
set -eu
stage=/opt/usr/home/owner/share/tmp/pebble-startup-install
dest=/opt/usr/home/owner/share/pebble-launcher
userdir=/opt/usr/home/owner/.config/systemd/user
runtime=/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024
payload=/opt/usr/home/owner/share/pebble
grep -q '^Model=SM-R850;' /etc/info.ini
grep -q '^Build=R850XXU1DWK2;' /etc/info.ini
test "$(id -u)" = 0
test ! -e /opt/usr/home/owner/share/pebble/enable-startup
for name in watch-device-bridge watch-sensor-idle watch-os-switch; do
    source=$stage/session/$name
    if [ -f "$source" ]; then
        chmod 0700 "$source"
    else
        source=$runtime/$name
    fi
    test "$("$source" --session-modes)" = zero=unlimited || {
        printf 'Unlimited-session helper required: %s\n' "$name" >&2
        exit 1
    }
done
runtime_scripts='start_phone_pairing_review.sh start_watch_device_bridge_review.sh
start_watch_sensor_idle_review.sh start_watch_os_switch_review.sh
start_watch_soak_review.sh start_watch_ui_guard_review.sh
start_watch_wear_review.sh relaunch_after_reboot.sh watch-soak watch-primary-ui-guard'
for name in $runtime_scripts; do
    file=$stage/session/$name
    test -r "$file"
    sh -n "$file"
done
backup=$dest.backup-$(date +%Y%m%dT%H%M%S)-$$
if [ -e "$dest" ]; then
    cp -a "$dest" "$backup"
fi
mkdir -p "$backup/runtime"
chmod 0700 "$backup"
for name in $runtime_scripts watch-device-bridge watch-sensor-idle watch-os-switch; do
    [ ! -f "$runtime/$name" ] || cp -p "$runtime/$name" "$backup/runtime/$name"
done
systemctl stop pebble-pairing-review.service pebble-device-bridge-review.service \
    pebble-sensor-idle-review.service pebble-ui-guard-review.service \
    pebble-os-switch-review.service pebble-soak-review.service || :
for name in pairing device-bridge sensor-idle ui-guard os-switch soak; do
    if systemctl is-active --quiet "pebble-$name-review.service"; then
        printf 'Previous service did not stop: %s\n' "$name" >&2
        exit 1
    fi
done
cp "$payload/pebbleos-flash.bin" "$backup/pebbleos-flash.bin"
chmod 0600 "$backup/pebbleos-flash.bin"
for name in $runtime_scripts watch-device-bridge watch-sensor-idle watch-os-switch; do
    file=$stage/session/$name
    [ -f "$file" ] || continue
    cp "$file" "$runtime/$name.new"
    chmod 0700 "$runtime/$name.new"
    mv "$runtime/$name.new" "$runtime/$name"
done
printf 'UNLIMITED_SESSION_BACKUP=%s\n' "$backup"
mkdir -p "$dest"
chown 0:0 "$dest"
chmod 0755 "$dest"
mkdir -p "$userdir"
for file in libcoreclr.so root-worker.so start-pebble; do
    cp "$stage/$file" "$dest/$file.new"
    chown 0:0 "$dest/$file.new"
    chmod 0644 "$dest/$file.new"
    mv "$dest/$file.new" "$dest/$file"
done
# Keep the root command path out of the owner's writable staging directory.
chmod 0700 "$dest/start-pebble"
if [ -f "$userdir/pebble-launcher.service" ]; then
    cp -p "$userdir/pebble-launcher.service" \
        "$userdir/pebble-launcher.service.backup-$(date +%Y%m%dT%H%M%S)"
fi
cp "$stage/pebble-launcher.service" "$userdir/pebble-launcher.service"
chown 5001:100 "$userdir" "$userdir/pebble-launcher.service"
chmod 0755 "$userdir"
chmod 0644 "$userdir/pebble-launcher.service"
printf 'PEBBLE_STARTUP_INSTALLED\n'
sha256sum "$dest/libcoreclr.so" "$dest/root-worker.so" "$dest/start-pebble"
