# Galaxy Watch3 connector for PebbleOS

PebbleOS runs on the Galaxy Watch3 (SM-R850) as an ordinary Linux process on top of Samsung's
kernel and Tizen userspace. This connector is the part that talks to Samsung's side. It is a
device adapter for the PebbleOS Linux port: it reads the accelerometer, heart-rate sensor and
pedometer, drives the haptic motor, controls display power and the always-on clock, and keeps the
sensor hub's FIFO from waking the processor.

PebbleOS and the connector speak the protocol described in `spec/adapter-protocol.md` of the
PebbleOS Linux port, over two Unix datagram sockets. The wire format is a single header,
`include/pbl/drivers/linux_adapter.h`, copied from the PebbleOS tree.

## Status

Experimental. The adapter builds and its logic is covered by host tests, but protocol version 2
has not run on a watch yet. The list of hardware checks is in the protocol spec under "Before
release". Battery life off the charger is not validated.

It does not flash anything, write to a partition, or enable itself at boot. A reboot returns the
watch to Samsung's software.

## What is here

| Path | What |
| --- | --- |
| `adapter/watch_device_bridge.c` | The adapter. ARM32, loads Tizen's libraries at run time. |
| `adapter/watch_sensor_idle.c` | Helper that buffers the sensor hub FIFO while the screen is off. |
| `adapter/watch_bridge_control.c` | Sends one protocol message to a running adapter, for debugging. |
| `adapter/conformance.c` | Checks an adapter against the protocol. Runs on the watch. |
| `adapter/test_*`, `adapter/run_adapter_tests.sh` | Unit tests that run on a Mac or Linux host. |
| `adapter/watch-wristup-rearm` | Re-enables Samsung wrist detection after a sensor-hub reset. |
| `include/pbl/drivers/linux_adapter.h` | The protocol header, from PebbleOS. |
| `profile/galaxy_watch3.profile` | The device profile PebbleOS uses for this watch. |
| `build.sh` | Builds the ARM32 binaries with `zig cc`. |

Samsung's libraries are never copied into this repository. The adapter opens the ones already on
the watch with `dlopen`.

## Build

```
ZIG=/path/to/zig sh build.sh      # binaries land in out/
```

The target is `arm-linux-gnueabi` with glibc 2.24, which matches Tizen 5.5.

## Test

```
sh adapter/run_adapter_tests.sh   # host tests, no watch needed
```

On the watch, with PebbleOS stopped and the adapter running:

```
out/conformance --dir /opt/usr/home/owner/share/pebble --full
```

## Requirements

A Galaxy Watch3 with developer mode on and a way to run code as the `owner` user over SDB. The
runtime directory is `/opt/usr/home/owner/share/pebble`; set `PBL_RUNTIME_DIR` for both PebbleOS
and the adapter to use another.

## Run

Copy the four programs from `out/` to the runtime directory using SDB, along with
`adapter/watch-wristup-rearm`. The helper needs that script beside its executable. Both
processes must use the same runtime directory as PebbleOS:

```sh
export PBL_RUNTIME_DIR=/opt/usr/home/owner/share/pebble
cd "$PBL_RUNTIME_DIR"
./watch_device_bridge 1200 >adapter.log 2>&1 &
adapter_pid=$!
./watch_sensor_idle 1200 >fifo.log 2>&1 &
fifo_pid=$!
```

The numeric argument is the run duration in seconds (30 to 86400). Start the matching v2
PebbleOS build after these processes. Stop them with `kill "$fifo_pid" "$adapter_pid"`.
They do not install a service or change boot settings. Stop the old v1 adapter before starting
this pair; its wire protocol is incompatible.

The device account needs access to Tizen's device and sensor APIs, `/dev/ssp_sensorhub`,
the system D-Bus, and wake alarms. Permission failures must be resolved on the device before
running a hardware test. This repository does not grant privileges or bypass device security.

## License

The connector uses [MIT](LICENSE). The shared PebbleOS protocol header and conformance checker
retain Apache-2.0; see [NOTICE](NOTICE) and `LICENSES/Apache-2.0.txt`.
