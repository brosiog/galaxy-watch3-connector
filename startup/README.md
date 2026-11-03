# PebbleOS automatic startup on Galaxy Watch3

Samsung's owner session starts the already installed PebbleOS runtime after
each boot. Charging and powering the watch back on no longer requires a Mac
launch command or a firmware build. The existing two-button hold switches
between Samsung and PebbleOS during the session.

This is specific to the owner's **SM-R850, R850XXU1DWK2** and its existing
PebbleOS installation. It uses a persistent user service in writable app data;
the Samsung boot image and system libraries stay unchanged. It retains the
installed review stack's **24-hour session limits** and runs once per boot.

## Install or update

From this directory, with authorized SDB connected, Zig available, and the
existing runtime installed:

```sh
PBL_TARGET=WATCH_IP:26101 ZIG=/path/to/zig SDB=/path/to/sdb sh install.sh
```

The installer builds two small ARM32 launcher libraries once, installs them with
the fixed startup script, starts the launcher, and enables the owner service.
It does not build PebbleOS. Repeating installation preserves a backup of the
previous launcher files and service definition.

This is an optional hook for an already configured development installation,
not an installer for a stock watch. It requires the existing root-managed
runtime under `/opt/usr/home/owner/share/pebble` and the tested relaunch script,
radio probe and console helper under
`/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024`. These development
runtime files are not shipped in this directory. Installation itself uses a
temporary owner service and removes its setup bootstrap afterward.

## How it works

The owner's systemd service manager retains the capabilities needed by Samsung's
`dotnet` host. The bootstrap opens a private mount namespace and temporarily
overlays a library used by the stock D-Bus launch helper. Its constructor enters
root, removes that overlay, and executes only the fixed startup script with a
fixed environment. The original library on disk remains intact. There is no
command socket, network listener, or caller-supplied root command.

`start-pebble` waits for Samsung's compositor and Bluetooth controller, then
uses the existing relaunch script, saved flash file and paired phone state.
It verifies all six runtime services and the UI executable. A concurrent request
exits without starting a second stack. A valid running stack is left running.
A failed launch stops its runtime services and asks Samsung's home app to open.

Files are stored in `/opt/usr/home/owner/share/pebble-launcher`; their owner is
root. The enclosing app-data location and user service configuration remain
under the trusted watch owner's control. The service becomes `active (exited)`
after startup and adds no polling daemon.

## Status, manual restart and disabling

From an authorized owner shell on the watch:

```sh
export XDG_RUNTIME_DIR=/run/user/5001
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/5001/bus
systemctl --user status pebble-launcher.service
systemctl --user restart pebble-launcher.service
```

To turn automatic startup off, use:

```sh
systemctl --user disable pebble-launcher.service
```

Disabling the boot hook leaves the current Pebble session running. Holding both
side buttons for approximately three seconds returns to Samsung. The startup log
is `pebble-launcher/startup.log` and is readable through the project's root worker.

## Verification

```sh
python3 test_startup.py
ZIG=/path/to/zig sh build.sh
```

Twenty-seven isolated tests cover successful startup, delayed or unavailable radio,
missing compositor, stale UI markers, repeated and concurrent requests, lock
errors, wrong hardware/firmware, missing dependencies, failed and falsely
successful legacy launches, partial service startup, Samsung recovery, and
starting the UI before the phone reconnects. Setup tests also verify fresh
installation logs, failed installation/restart handling, and removal of the
temporary root bootstrap. Tests run against a temporary
filesystem and mocked device commands; they do not operate the watch.

On November 3, 2026, the permanent user service successfully ran on the watch.
After enabling it and rebooting, the owner confirmed PebbleOS appeared without
any Mac launch command. The runtime executable is the existing installed build,
SHA-256 `f5d5d017dd91b5157ba39faac4c94cd76b5ded630887c074c6d9d1a25947c2ae`.
Detailed transcripts are retained locally in
`analysis/load-pebbleos-20261103/` and `evidence/`.

The boot log records startup at 33.59 seconds uptime and a connected phone.
The owner also confirmed the return to Samsung, and the runtime logged the
two-button hold and exited with status zero. The final standalone installer
was then exercised against that saved installation.
