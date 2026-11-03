"""Advance the real shell helpers' clocks past one day without device I/O."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

MOCK = r'''#!/usr/bin/env python3
import json, os, pathlib, signal, sys
root = pathlib.Path(os.environ["FIXTURE"])
statefile = root / "state.json"
state = json.loads(statefile.read_text())
name = sys.argv[1]
if name == "cut":
    print(state["now"] if "uptime" in sys.argv[-1] else 0)
elif name == "readlink":
    print(str(root / "payload/pebbleos-pairing-debug"))
elif name == "sleep":
    state["sleeps"] += 1
    state["now"] += 100000
statefile.write_text(json.dumps(state))
if name == "sleep" and state["sleeps"] == 2:
    os.kill(os.getppid(), signal.SIGTERM)
'''


class RuntimeSessionTest(unittest.TestCase):
    def run_helper(self, name, duration):
        with tempfile.TemporaryDirectory(prefix="pbl-session-", dir="/tmp") as temp:
            root = Path(temp)
            (root / "payload").mkdir()
            (root / "bin").mkdir()
            (root / "payload/pebble-ui-active").write_text("1234\n")
            (root / "state.json").write_text(json.dumps({"now": 100, "sleeps": 0}))
            mock = root / "mock.py"
            mock.write_text(MOCK)
            mock.chmod(0o755)
            for command in ("cut", "sleep", "pgrep", "readlink", "systemctl"):
                wrapper = root / "bin" / command
                wrapper.write_text(f'#!/bin/sh\nexec "{mock}" {command} "$@"\n')
                wrapper.chmod(0o755)
            text = (Path(__file__).parent / "runtime" / name).read_text()
            text = text.replace("/opt/usr/home/owner/share/pebble", str(root / "payload"))
            text = text.replace("/sys/", str(root / "sys") + "/")
            text = text.replace("/proc/", str(root / "proc") + "/")
            script = root / "helper.sh"
            script.write_text(text)
            env = dict(os.environ, FIXTURE=str(root),
                       PATH=str(root / "bin") + os.pathsep + os.environ["PATH"])
            result = subprocess.run(["/bin/sh", str(script), str(duration)],
                                    env=env, capture_output=True, text=True, timeout=8)
            state = json.loads((root / "state.json").read_text())
            # Sleep advances the clock by 100,000 seconds, already beyond a day.
            self.assertEqual(state["sleeps"], 2 if duration == 0 else 1, result)
            self.assertFalse((root / "payload/primary-ui-paused").exists())
            if name == "watch-soak":
                log = (root / "payload/soak.log").read_text()
                self.assertEqual("SOAK_END" in log, duration != 0)
            else:
                self.assertEqual(result.returncode, 0, result)

    def test_unlimited_telemetry_survives_one_day_and_stops_on_signal(self):
        self.run_helper("watch-soak", 0)

    def test_bounded_telemetry_still_expires(self):
        self.run_helper("watch-soak", 30)

    def test_unlimited_ui_guard_survives_one_day_and_restores_on_signal(self):
        self.run_helper("watch-primary-ui-guard", 0)

    def test_bounded_ui_guard_still_expires_and_restores(self):
        self.run_helper("watch-primary-ui-guard", 30)


if __name__ == "__main__":
    unittest.main(verbosity=2)
