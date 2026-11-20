#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#160: the test-only HFP operator tool verifies HFP itself and fails loudly.

busctl, sudo and pw-dump are stubbed on PATH; no Bluetooth, PipeWire or root is touched.
"""
from __future__ import annotations

import json
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

BUSCTL = r'''#!/usr/bin/env bash
state=$AA_STUB_DIR
case "$1 $2" in
  "--list tree") printf '/\n/org/bluez/hci0\n%s\n' "$AA_STUB_DEVICE"; exit 0 ;;
esac
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

    def test_connect_verified_by_hfp_profile(self) -> None:
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("HFP=up", result.stdout)
        self.assertEqual(self.calls(), ["ConnectProfile 0000111f-0000-1000-8000-00805f9b34fb"])

    def test_connect_without_hfp_profile_fails(self) -> None:
        (self.stub / "connect").write_text("le")  # Connected=true over LE, no HFP
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no HFP profile", result.stderr)

    def test_connect_error_fails_with_error_name_only(self) -> None:
        (self.stub / "connect").write_text("fail")
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("org.bluez.Error.Failed", result.stderr)

    def test_unreadable_graph_fails(self) -> None:
        (self.stub / "pw-broken").touch()
        self.assertEqual(self.run_tool("connect").returncode, 1)

    def test_disconnect_verified(self) -> None:
        (self.stub / "hfp").touch()
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("HFP=down", result.stdout)

    def test_failed_disconnect_exits_nonzero(self) -> None:
        (self.stub / "hfp").touch()
        (self.stub / "disconnect").write_text("fail")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("DisconnectProfile(HFP AG) failed", result.stderr)

    def test_disconnect_that_leaves_hfp_up_fails(self) -> None:
        (self.stub / "hfp").touch()
        (self.stub / "disconnect").write_text("stuck")
        self.assertEqual(self.run_tool("disconnect").returncode, 1)

    def test_empty_graph_cannot_prove_a_disconnect(self) -> None:
        (self.stub / "pw-empty").touch()  # the graph sees no Bluetooth at all
        for mode in ("stuck", "down"):
            (self.stub / "disconnect").write_text(mode)
            result = self.run_tool("disconnect")
            self.assertEqual(result.returncode, 1, mode)
            self.assertIn("unverifiable", result.stderr)

    def test_empty_graph_cannot_prove_a_connect(self) -> None:
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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
