#!/bin/sh
# Run by the temporary setup bootstrap through the owner's service manager.
set -eu
stage=/opt/usr/home/owner/share/tmp/pebble-startup-install
dest=/opt/usr/home/owner/share/pebble-launcher
userdir=/opt/usr/home/owner/.config/systemd/user
grep -q '^Model=SM-R850;' /etc/info.ini
grep -q '^Build=R850XXU1DWK2;' /etc/info.ini
test "$(id -u)" = 0
test ! -e /opt/usr/home/owner/share/pebble/enable-startup
if [ -e "$dest" ]; then
    cp -a "$dest" "$dest.backup-$(date +%Y%m%dT%H%M%S)"
fi
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
