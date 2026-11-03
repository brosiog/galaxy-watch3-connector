# Galaxy Watch3 connector for PebbleOS

This connector gives the [PebbleOS Linux port](https://github.com/brosiog/pebbleos-linux) access to Samsung
Galaxy Watch3 hardware through the Tizen libraries already installed on the watch. It runs as
a separate ARM32 process and exchanges protocol-v2 messages with PebbleOS over Unix datagram
sockets. The OS can use either the AArch64 or ARM32 build.

The connector implements accelerometer, heart-rate and step delivery, haptic commands,
brightness and display-power control. A helper manages sensor-hub buffering while the display
is off. PebbleOS owns UI, display timeouts and gesture policy; the connector executes those
requests and reports device state.

## Status and requirements

Experimental source candidate. Host tests and ARM32 cross-builds pass. **Protocol v2 has not
yet run on the watch**, and battery endurance, sensor accuracy, suspend behavior and display
timing remain unverified for this release.

The reference device is the **Galaxy Watch3 SM-R850 running Tizen 5.5.0.2**. Other models and
Tizen versions need separate validation. Device testing needs SDB access and an account with
permission to use Tizen's sensor and device APIs, `/dev/ssp_sensorhub`, the system D-Bus and wake
alarms. This repository does not configure those permissions.

Use a matching protocol-v2 PebbleOS build. Stop any old v1 adapter before starting this one.
The connector does not flash partitions, install a service or change boot settings.

## Build on a host

Install Zig and run from this repository. The candidate was built with Zig 0.17.0:

```sh
ZIG=/path/to/zig sh build.sh
```

The build produces four programs in `out/`: `watch_device_bridge`, `watch_sensor_idle`,
`watch_bridge_control` and `conformance`. The target is ARM32 Linux with glibc 2.24 and
`/lib/ld-linux.so.3`, matching the reference device's Tizen userspace. The PebbleOS source tree
and Tizen SDK are not needed to build the connector. Samsung libraries are loaded with `dlopen`
on the watch and are excluded from this repository.

## Run host tests

Use a C11 compiler with pthreads on macOS or Linux:

```sh
sh adapter/run_adapter_tests.sh
```

The tests simulate vendor APIs and cover sensor delivery, FIFO policy, haptic deadlines and
control-tool argument handling. They need neither Zig nor a watch and do not establish hardware
power consumption or timing.

## Run on a development watch

Build the matching PebbleOS Linux port separately. Copy the four programs from `out/` to a
writable runtime directory using SDB. Also copy `adapter/watch-wristup-rearm` beside
`watch_sensor_idle`, keeping the script's filename. Make the copied programs and script executable.

On the watch, start the connector and helper:

```sh
export PBL_RUNTIME_DIR=/opt/usr/home/owner/share/pebble
cd "$PBL_RUNTIME_DIR"
./watch_device_bridge 1200 >adapter.log 2>&1 &
adapter_pid=$!
./watch_sensor_idle 1200 >fifo.log 2>&1 &
fifo_pid=$!
```

The numeric argument is the run duration in seconds, from 30 to 86400. These commands run for
20 minutes. Start PebbleOS with the same `PBL_RUNTIME_DIR`; both processes default to the path
shown above. See [device profiles](spec/device-profile.md) for directory and input configuration.

Stop the helper and connector with:

```sh
kill "$fifo_pid" "$adapter_pid"
```

For protocol checks, stop PebbleOS, leave the connector running and use the copied checker:

```sh
./conformance --dir "$PBL_RUNTIME_DIR" --full
```

The [protocol specification](spec/adapter-protocol.md#before-release) lists the remaining
hardware release checks, including wake latency, suspend, haptics, sensor quality and recovery.

## Source map

| Path | Purpose |
| --- | --- |
| `adapter/watch_device_bridge.c` | Device adapter; loads Tizen's libraries at runtime. |
| `adapter/watch_sensor_idle.c` | Sensor-hub FIFO policy helper. |
| `adapter/watch_bridge_control.c` | Sends one protocol message for debugging. |
| `adapter/conformance.c` | Protocol conformance checker. |
| `adapter/test_*`, `adapter/run_adapter_tests.sh` | Host tests and their runner. |
| `adapter/watch-wristup-rearm` | Restores wrist detection after a sensor-hub reset. |
| `include/pbl/drivers/linux_adapter.h` | Shared wire-format header, copied from PebbleOS. |
| `profile/galaxy_watch3.profile` | Reference device profile. |
| `build.sh` | ARM32 cross-build script. |

For another Linux watch, implement its vendor API calls behind the same protocol. Keep
OS policy in PebbleOS and device-specific recovery in the adapter.

## License

The connector uses [MIT](LICENSE). The copied PebbleOS protocol header and conformance checker
retain Apache-2.0; see [NOTICE](NOTICE) and [the Apache license](LICENSES/Apache-2.0.txt).

## Optional automatic startup

An already configured SM-R850 development watch can start its installed PebbleOS
runtime after reboot using the optional [startup hook](startup/README.md).
The hook has its own build and install commands and is separate from the
connector binaries. It requires the existing development runtime and helpers;
it does not set up a stock watch.
