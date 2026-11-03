"""Exercise startup decisions and recovery without touching a real watch."""
import fcntl
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).with_name("start-pebble")
MOCK = r'''#!/usr/bin/env python3
import fcntl, json, os, pathlib, sys
root = pathlib.Path(os.environ["FIXTURE"])
path = root / "state.json"
state = json.loads(path.read_text())
name, *args = sys.argv[1:]
state["events"].append([name, *args])
rc = 0
if name == "id":
    print(state.get("uid", 0))
elif name == "flock":
    rc = state.get("lock_error", 0)
    if rc == 0:
        try:
            fcntl.flock(int(args[-1]), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            rc = 1
elif name == "readlink":
    print(str(root / "payload/pebbleos-pairing-debug") if state.get("valid_pid") else "/old/process")
elif name == "systemctl":
    args = [a for a in args if a != "--user"]
    if args[0] == "is-active":
        rc = 0 if args[-1] in state["active"] else 3
    elif args[0] == "stop":
        state["active"] = []
    elif args[0] == "start" and args[-1] == "pebble-launcher-install.service":
        rc = state.get("setup_rc", 0)
        if rc == 0 and not state.get("setup_silent_failure"):
            (root / "stage/owner-install.log").write_text("PEBBLE_STARTUP_INSTALLED\n")
    elif args[0] == "restart":
        rc = state.get("restart_rc", 0)
elif name == "hci":
    state["probes"] = state.get("probes", 0) + 1
    rc = 0 if state["probes"] > state.get("not_ready", 0) else 1
elif name == "relaunch":
    state["session_seconds"] = os.environ.get("PBL_SESSION_SECONDS")
    rc = state.get("launch_rc", 0)
    if rc == 0 and not state.get("silent_failure"):
        state["active"] = state["required"][:]
        if state.get("missing_service") in state["active"]:
            state["active"].remove(state["missing_service"])
        state["valid_pid"] = True
        (root / "payload/pebble-ui-active").write_text("1234\n")
elif name == "console":
    print("app name: Internal\nAlive: yes\nConnected: " + state.get("connected", "yes"))
path.write_text(json.dumps(state))
sys.exit(rc)
'''


class StartupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pbl-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in ("bin", "launcher", "payload", "stage", "run"):
            (self.root / directory).mkdir()
        self.state = {
            "events": [], "active": [], "valid_pid": False,
            "required": [f"pebble-{name}-review.service" for name in (
                "pairing", "device-bridge", "os-switch", "sensor-idle", "ui-guard", "soak")],
        }
        (self.root / "info.ini").write_text("Model=SM-R850;\nBuild=R850XXU1DWK2;\n")
        (self.root / "uptime").write_text("30.0 20.0\n")
        mock = self.root / "mock.py"
        mock.write_text(MOCK)
        mock.chmod(0o755)
        for command, target in (("id", "id"), ("flock", "flock"), ("readlink", "readlink"),
                                ("systemctl", "systemctl"), ("sleep", "sleep"),
                                ("app_launcher", "home")):
            self.wrapper(self.root / "bin" / command, target)
        self.wrapper(self.root / "stage/hci-ready-probe", "hci")
        self.wrapper(self.root / "stage/watch-console-probe", "console")
        self.wrapper(self.root / "stage/relaunch_after_reboot.sh", "relaunch")
        runtime = self.root / "payload/pebbleos-pairing-debug"
        runtime.touch()
        runtime.chmod(0o700)
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.bind(str(self.root / "run/wayland-0"))
        self.addCleanup(self.socket.close)
        script = SOURCE.read_text()
        for old, new in (
            ("/opt/usr/home/owner/share/pebble-launcher", "launcher"),
            ("/opt/usr/home/owner/share/pebble", "payload"),
            ("/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024", "stage"),
            ("/etc/info.ini", "info.ini"),
            ("/proc/uptime", "uptime"),
            ("/run/wayland-0", "run/wayland-0"),
            ("/usr/bin/app_launcher", "bin/app_launcher"),
        ):
            script = script.replace(old, str(self.root / new))
        self.script = self.root / "start-pebble"
        self.script.write_text(script)

    def wrapper(self, path, target):
        # Paths are tempfile-generated and contain no shell metacharacters.
        path.write_text(f'#!/bin/sh\nexec "{self.root}/mock.py" {target} "$@"\n')
        path.chmod(0o755)

    def run_launcher(self, success=True):
        (self.root / "state.json").write_text(json.dumps(self.state))
        env = dict(os.environ, FIXTURE=str(self.root),
                   PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["/bin/sh", str(self.script)], env=env,
                                capture_output=True, text=True, timeout=15)
        self.state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(result.returncode == 0, success,
                         (result, (self.root / "launcher/startup.log").read_text()))
        return (self.root / "launcher/startup.log").read_text()

    def events(self, name):
        return [event for event in self.state["events"] if event[0] == name]

    def run_setup(self, success=True):
        script = SOURCE.with_name("setup-owner.sh").read_text()
        script = script.replace("/opt/usr/home/owner/share/tmp/pebble-startup-install",
                                str(self.root / "stage"))
        script = script.replace("/run/user/5001", str(self.root / "run/user"))
        path = self.root / "setup-owner.sh"
        path.write_text(script)
        (self.root / "stage/installer").mkdir()
        for p in ("installer/libcoreclr.so", "install-worker.so", "install-on-watch.sh"):
            (self.root / "stage" / p).touch()
        (self.root / "state.json").write_text(json.dumps(self.state))
        env = dict(os.environ, FIXTURE=str(self.root),
                   PATH=str(self.root / "bin") + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["/bin/sh", str(path)], env=env,
                                capture_output=True, text=True, timeout=15)
        self.state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(result.returncode == 0, success, result)
        self.assertFalse((self.root / "stage/installer/libcoreclr.so").exists())
        self.assertFalse((self.root / "stage/install-worker.so").exists())
        self.assertFalse((self.root / "stage/install-on-watch.sh").exists())
        return result

    def assert_recovered(self):
        self.assertFalse(self.state["active"])
        self.assertTrue(self.events("home"))

    def test_successful_cold_start(self):
        self.assertIn("PEBBLE_STARTUP_OK pid=1234", self.run_launcher())
        self.assertEqual(len(self.events("relaunch")), 1)
        self.assertEqual(self.state["active"], self.state["required"])
        self.assertEqual(self.state["session_seconds"], "0")

    def test_delayed_radio_readiness(self):
        self.state["not_ready"] = 4
        self.run_launcher()
        self.assertEqual(self.state["probes"], 5)
        self.assertEqual(len(self.events("sleep")), 4)
        self.assertEqual(len(self.events("relaunch")), 1)

    def test_radio_timeout_restores_samsung(self):
        self.state["not_ready"] = 999
        self.run_launcher(success=False)
        self.assertEqual(self.state["probes"], 60)
        self.assertFalse(self.events("relaunch"))
        self.assert_recovered()

    def test_missing_compositor_times_out_without_launch(self):
        self.socket.close()
        (self.root / "run/wayland-0").unlink()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))
        self.assert_recovered()

    def test_running_stack_is_not_restarted(self):
        self.state["active"] = self.state["required"][:]
        self.state["valid_pid"] = True
        (self.root / "payload/pebble-ui-active").write_text("1234\n")
        self.assertIn("PEBBLE_ALREADY_RUNNING", self.run_launcher())
        self.assertFalse(self.events("relaunch"))
        self.assertFalse(self.events("home"))

    def test_stale_pid_is_not_accepted(self):
        self.state["active"] = self.state["required"][:]
        (self.root / "payload/pebble-ui-active").write_text("1234\n")
        self.assertIn("PEBBLE_STARTUP_OK", self.run_launcher())
        self.assertEqual(len(self.events("relaunch")), 1)

    def test_missing_pid_marker_is_not_accepted(self):
        self.state["active"] = self.state["required"][:]
        self.state["valid_pid"] = True
        self.assertIn("PEBBLE_STARTUP_OK", self.run_launcher())
        self.assertEqual(len(self.events("relaunch")), 1)

    def test_running_ui_with_dead_helper_is_relaunched(self):
        self.state["active"] = self.state["required"][:]
        self.state["active"].remove("pebble-ui-guard-review.service")
        self.state["valid_pid"] = True
        (self.root / "payload/pebble-ui-active").write_text("1234\n")
        self.assertIn("PEBBLE_STARTUP_OK", self.run_launcher())
        self.assertEqual(len(self.events("relaunch")), 1)

    def test_second_request_after_success_does_not_restart(self):
        self.run_launcher()
        self.assertIn("PEBBLE_ALREADY_RUNNING", self.run_launcher())
        self.assertEqual(len(self.events("relaunch")), 1)

    def test_nonzero_launch_exit_restores_samsung(self):
        self.state["launch_rc"] = 42
        self.run_launcher(success=False)
        self.assert_recovered()

    def test_false_success_from_legacy_launcher_is_detected(self):
        self.state["silent_failure"] = True
        self.run_launcher(success=False)
        self.assert_recovered()

    def test_incomplete_stack_restores_samsung(self):
        self.state["missing_service"] = "pebble-sensor-idle-review.service"
        self.run_launcher(success=False)
        self.assert_recovered()

    def test_phone_reconnection_is_not_required_to_show_ui(self):
        self.state["connected"] = "no"
        self.assertIn("PEBBLE_STARTUP_OK", self.run_launcher())

    def test_wrong_watch_is_rejected_without_side_effects(self):
        (self.root / "info.ini").write_text("Model=SM-R800;\nBuild=R850XXU1DWK2;\n")
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))
        self.assertFalse(self.events("home"))

    def test_wrong_firmware_is_rejected_without_side_effects(self):
        (self.root / "info.ini").write_text("Model=SM-R850;\nBuild=OTHER;\n")
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_missing_runtime_is_rejected(self):
        (self.root / "payload/pebbleos-pairing-debug").unlink()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_missing_radio_probe_is_rejected(self):
        (self.root / "stage/hci-ready-probe").unlink()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_missing_console_probe_is_rejected(self):
        (self.root / "stage/watch-console-probe").unlink()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_missing_relaunch_script_is_rejected(self):
        (self.root / "stage/relaunch_after_reboot.sh").unlink()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_legacy_boot_flag_is_rejected(self):
        (self.root / "payload/enable-startup").touch()
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_unprivileged_invocation_is_rejected(self):
        self.state["uid"] = 5001
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_concurrent_invocation_is_a_noop(self):
        with (self.root / "launcher/launch.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_launcher()
            self.assertFalse(self.events("relaunch"))

    def test_lock_error_is_not_reported_as_success(self):
        self.state["lock_error"] = 2
        self.run_launcher(success=False)
        self.assertFalse(self.events("relaunch"))

    def test_setup_enables_only_after_install_and_launch(self):
        self.assertIn("PEBBLE_STARTUP_ENABLED", self.run_setup().stdout)
        commands = [e[2] for e in self.events("systemctl")]
        self.assertLess(commands.index("start"), commands.index("restart"))
        self.assertLess(commands.index("restart"), commands.index("enable"))

    def test_failed_setup_removes_temporary_root_bootstrap(self):
        self.state["setup_rc"] = 1
        self.run_setup(success=False)
        self.assertFalse(any("enable" in e for e in self.events("systemctl")))

    def test_stale_setup_log_cannot_report_success(self):
        (self.root / "stage/owner-install.log").write_text("PEBBLE_STARTUP_INSTALLED\n")
        self.state["setup_silent_failure"] = True
        self.run_setup(success=False)
        self.assertFalse(any("restart" in e for e in self.events("systemctl")))

    def test_failed_restart_does_not_enable_boot(self):
        self.state["restart_rc"] = 1
        self.run_setup(success=False)
        self.assertFalse(any("enable" in e for e in self.events("systemctl")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
