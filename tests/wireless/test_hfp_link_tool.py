#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#160/#179: the test-only HFP operator tool verifies HFP via BlueZ D-Bus and fails loudly.

busctl, sudo and pw-dump are stubbed on PATH; no Bluetooth, PipeWire or root is touched.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/wireless-qa/aa-hfp-link-test-only.sh"
DEVICE = "/org/bluez/hci0/dev_02_00_00_00_00_01"
ADDRESS = "02:00:00:00:00:01"
HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb"

BUSCTL = r'''#!/usr/bin/env bash
state=$AA_STUB_DIR
case "$1 $2" in
  "--list tree") printf '/\n/org/bluez/hci0\n%s\n' "$AA_STUB_DEVICE"; exit 0 ;;
esac
if [ "$1" = "--json=short" ] && [ "$2" = "call" ] && [ "$6" = "GetManagedObjects" ]; then
  [ -f "$state/bluez-broken" ] && { echo "not-json"; exit 0; }
  [ -f "$state/bluez-fail" ] && { echo "Call failed" >&2; exit 1; }
  python3 -I -c '
import json, os
from pathlib import Path

state = Path(os.environ["AA_STUB_DIR"])
dev_path = os.environ["AA_STUB_DEVICE"]
hfp_ag = "0000111f-0000-1000-8000-00805f9b34fb"
a2dp = "0000110a-0000-1000-8000-00805f9b34fb"

conn_text = (state / "connected").read_text().strip() if (state / "connected").exists() else "b false"
connected = conn_text == "b true"
hfp_up = (state / "hfp").exists()
mode = (state / "bluez-mode").read_text().strip() if (state / "bluez-mode").exists() else "profile"

dev_props = {
    "Alias": {"type": "s", "data": "Test Phone"},
    "Paired": {"type": "b", "data": True},
    "Connected": {"type": "b", "data": connected},
}
objs = {dev_path: {"org.bluez.Device1": dev_props}}

if mode == "profile":
    dev_props["ConnectedProfiles"] = {"type": "as", "data": [hfp_ag] if hfp_up else []}
elif mode == "service":
    objs[f"{dev_path}/service0"] = {
        "org.bluez.Service1": {
            "Device": {"type": "o", "data": dev_path},
            "UUID": {"type": "s", "data": hfp_ag},
            "State": {"type": "s", "data": "connected" if hfp_up else "disconnected"},
        }
    }
elif mode == "transport":
    if hfp_up:
        objs[f"{dev_path}/fd0"] = {
            "org.bluez.MediaTransport1": {
                "Device": {"type": "o", "data": dev_path},
                "UUID": {"type": "s", "data": hfp_ag},
                "State": {"type": "s", "data": "idle"},
            }
        }
elif mode == "a2dp-transport-only":
    objs[f"{dev_path}/fd0"] = {
        "org.bluez.MediaTransport1": {
            "Device": {"type": "o", "data": dev_path},
            "UUID": {"type": "s", "data": a2dp},
            "State": {"type": "s", "data": "active"},
        }
    }
elif mode == "none":
    pass

print(json.dumps({"type": "a{oa{sa{sv}}}", "data": [objs]}))
'
  exit 0
fi
if [ "$1" = get-property ]; then
  case "$5" in
    Alias) echo 's "Test Phone"' ;;
    Paired) echo 'b true' ;;
    Connected) cat "$state/connected" 2>/dev/null || echo 'b false' ;;
  esac
  exit 0
fi
if [ "$1" = call ]; then
  echo "$5 $7" >> "$state/calls"
  case "$5" in
    ConnectProfile) mode=$(cat "$state/connect"); ;;
    DisconnectProfile) mode=$(cat "$state/disconnect"); ;;
    *) exit 9 ;;
  esac
  case "$mode" in
    hfp) echo 'b true' > "$state/connected"; touch "$state/hfp" ;;
    le) echo 'b true' > "$state/connected" ;;
    down) echo 'b false' > "$state/connected"; rm -f "$state/hfp" ;;
    down-keep-link) echo 'b true' > "$state/connected"; rm -f "$state/hfp" ;;
    stuck) ;;
    notconnected) echo "Call failed: org.bluez.Error.NotConnected" >&2; exit 1 ;;
    *) echo "Call failed: org.bluez.Error.Failed" >&2; exit 1 ;;
  esac
  exit 0
fi
exit 9
'''
SUDO = '#!/usr/bin/env bash\n[ "$1" = -n ] && shift\n[ "$1" = timeout ] && shift 2\nexec "$@"\n'
PW_DUMP = r'''#!/usr/bin/env bash
[ -f "$AA_STUB_DIR/pw-broken" ] && { echo not-json; exit 0; }
[ -f "$AA_STUB_DIR/pw-empty" ] && { echo '[]'; exit 0; }
if [ -f "$AA_STUB_DIR/hfp" ]; then profile=headset-head-unit; else profile=off; fi
printf '[{"type":"PipeWire:Interface:Device","info":{"props":{"device.api":"bluez5","api.bluez5.address":"%s"},"params":{"Profile":[{"name":"%s"}]}}}]\n' "$AA_STUB_ADDRESS" "$profile"
'''


class HfpLinkTool(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.stub = Path(self.dir.name)
        for name, body in (("busctl", BUSCTL), ("sudo", SUDO), ("pw-dump", PW_DUMP)):
            path = self.stub / name
            path.write_text(body)
            path.chmod(0o700)
        (self.stub / "connect").write_text("hfp")
        (self.stub / "disconnect").write_text("down")
        (self.stub / "bluez-mode").write_text("profile")

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_tool(self, action: str) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, PATH=f"{self.stub}:{os.environ['PATH']}", AA_STUB_DIR=str(self.stub),
                   AA_STUB_DEVICE=DEVICE, AA_STUB_ADDRESS=ADDRESS, AA_HFP_VERIFY_S="2",
                   XDG_RUNTIME_DIR=str(self.stub))
        result = subprocess.run(["bash", str(TOOL), action, "Test Phone"], env=env, text=True,
                                capture_output=True, timeout=30, check=False)
        self.assertNotIn(ADDRESS, result.stdout + result.stderr, "the tool printed an address")
        return result

    def calls(self) -> list[str]:
        path = self.stub / "calls"
        return path.read_text().splitlines() if path.exists() else []

    def test_connect_verified_by_bluez_profile_even_when_pipewire_empty_or_broken(self) -> None:
        for pw_flag in ("pw-empty", "pw-broken"):
            with self.subTest(pw_flag=pw_flag):
                for p in ("pw-empty", "pw-broken", "hfp", "connected", "calls"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / pw_flag).touch()
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("HFP=up Connected=b true"), result.stdout)
                self.assertEqual(self.calls(), [f"ConnectProfile {HFP_AG}"])

    def test_connect_and_disconnect_verified_by_bluez_service_and_transport(self) -> None:
        for mode in ("service", "transport"):
            with self.subTest(mode=mode):
                for p in ("hfp", "connected", "calls"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "bluez-mode").write_text(mode)
                (self.stub / "pw-empty").touch()
                res_up = self.run_tool("connect")
                self.assertEqual(res_up.returncode, 0, res_up.stderr)
                self.assertTrue(res_up.stdout.startswith("HFP=up Connected=b true"), res_up.stdout)

                # Disconnect while another profile keeps Device1.Connected=true.
                (self.stub / "disconnect").write_text("down-keep-link")
                res_down = self.run_tool("disconnect")
                self.assertEqual(res_down.returncode, 0, res_down.stderr)
                self.assertTrue(res_down.stdout.startswith("HFP=down Connected=b true"), res_down.stdout)

    def test_status_reports_parseable_prefix_from_bluez(self) -> None:
        (self.stub / "pw-empty").touch()
        (self.stub / "connected").write_text("b true")
        (self.stub / "hfp").touch()
        res_up = self.run_tool("status")
        self.assertEqual(res_up.returncode, 0, res_up.stderr)
        self.assertTrue(res_up.stdout.startswith("HFP=up Connected=b true"), res_up.stdout)

        (self.stub / "hfp").unlink()
        (self.stub / "connected").write_text("b false")
        res_down = self.run_tool("status")
        self.assertEqual(res_down.returncode, 0, res_down.stderr)
        self.assertTrue(res_down.stdout.startswith("HFP=down Connected=b false"), res_down.stdout)

    def test_connect_without_hfp_profile_fails(self) -> None:
        for mode in ("profile", "service", "transport", "a2dp-transport-only", "none"):
            with self.subTest(mode=mode):
                for p in ("hfp", "connected", "calls"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "bluez-mode").write_text(mode)
                (self.stub / "pw-empty").touch()
                (self.stub / "connect").write_text("le")  # Connected=true over LE/A2DP, no HFP
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 1)
                self.assertIn("no HFP profile", result.stderr)

    def test_connect_error_fails_with_error_name_only(self) -> None:
        (self.stub / "connect").write_text("fail")
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("org.bluez.Error.Failed", result.stderr)

    def test_unreadable_bluez_dbus_fails(self) -> None:
        for flag in ("bluez-broken", "bluez-fail"):
            with self.subTest(flag=flag):
                for f in ("bluez-broken", "bluez-fail"):
                    (self.stub / f).unlink(missing_ok=True)
                (self.stub / flag).touch()
                self.assertEqual(self.run_tool("connect").returncode, 1)
                self.assertEqual(self.run_tool("status").returncode, 1)

    def test_pipewire_secondary_fallback_works_when_bluez_has_no_profile_state(self) -> None:
        (self.stub / "bluez-mode").write_text("none")
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("HFP=up Connected=b true"), result.stdout)
        self.assertIn("source=pipewire", result.stdout)

    def test_disconnect_verified(self) -> None:
        (self.stub / "connected").write_text("b true")
        (self.stub / "hfp").touch()
        (self.stub / "pw-empty").touch()
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("HFP=down Connected=b false"), result.stdout)

    def test_failed_disconnect_exits_nonzero(self) -> None:
        (self.stub / "connected").write_text("b true")
        (self.stub / "hfp").touch()
        (self.stub / "disconnect").write_text("fail")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("DisconnectProfile(HFP AG) failed", result.stderr)

    def test_disconnect_that_leaves_hfp_up_fails(self) -> None:
        (self.stub / "connected").write_text("b true")
        (self.stub / "hfp").touch()
        (self.stub / "disconnect").write_text("stuck")
        self.assertEqual(self.run_tool("disconnect").returncode, 1)

    def test_absent_bluez_and_pipewire_state_cannot_prove_disconnect_when_still_connected(self) -> None:
        (self.stub / "bluez-mode").write_text("none")
        (self.stub / "connected").write_text("b true")
        (self.stub / "pw-empty").touch()
        for mode in ("stuck", "down-keep-link"):
            (self.stub / "disconnect").write_text(mode)
            result = self.run_tool("disconnect")
            self.assertEqual(result.returncode, 1, mode)
            self.assertIn("unverifiable", result.stderr)

    def test_absent_bluez_and_pipewire_state_cannot_prove_connect(self) -> None:
        (self.stub / "bluez-mode").write_text("none")
        (self.stub / "pw-empty").touch()
        self.assertEqual(self.run_tool("connect").returncode, 1)

    def test_already_down_is_success(self) -> None:
        (self.stub / "disconnect").write_text("notconnected")
        self.assertEqual(self.run_tool("disconnect").returncode, 0)

    def test_only_profile_scoped_calls(self) -> None:
        self.run_tool("connect")
        self.run_tool("disconnect")
        for call in self.calls():
            self.assertRegex(call, r"^(Connect|Disconnect)Profile 0000111f-")
            self.assertNotRegex(call, r"RemoveDevice|CancelPairing|^Connect |^Disconnect ")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
