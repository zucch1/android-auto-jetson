#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#160/#179: the test-only HFP operator tool verifies HFP via bluetoothd journal and fails loudly.

busctl, sudo, journalctl and pw-dump are stubbed on PATH; no Bluetooth, PipeWire or root is touched.
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
OTHER_ADDRESS = "02:00:00:00:00:99"
HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb"

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
    hfp)
      echo 'b true' > "$state/connected"
      touch "$state/hfp"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: disconnected -> connecting (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    hfp-then-drop)
      echo 'b true' > "$state/connected"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connected -> disconnecting (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    le)
      echo 'b true' > "$state/connected"
      printf 'src/service.c:change_state() 0xaaaa2222: device %s profile gap-profile state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    no-debug)
      echo 'b true' > "$state/connected"
      printf 'bluetoothd[999]: Endpoint registered\n' >> "$state/journal"
      ;;
    down)
      echo 'b false' > "$state/connected"
      rm -f "$state/hfp"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connected -> disconnecting (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    down-keep-link)
      echo 'b true' > "$state/connected"
      rm -f "$state/hfp"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connected -> disconnecting (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    down-bearer-only)
      echo 'b false' > "$state/connected"
      rm -f "$state/hfp"
      ;;
    stuck-keep-link)
      echo 'b true' > "$state/connected"
      ;;
    stuck-connected)
      echo 'b true' > "$state/connected"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    notconnected) echo "Call failed: org.bluez.Error.NotConnected" >&2; exit 1 ;;
    *) echo "Call failed: org.bluez.Error.Failed" >&2; exit 1 ;;
  esac
  exit 0
fi
exit 9
'''
SUDO = '#!/usr/bin/env bash\n[ "$1" = -n ] && shift\n[ "$1" = timeout ] && shift 2\nexec "$@"\n'
JOURNALCTL = r'''#!/usr/bin/env bash
state=$AA_STUB_DIR
echo "$*" >> "$state/journalctl-args"
[ -f "$state/journal-unreadable" ] && { echo "No journal files were opened due to insufficient permissions." >&2; exit 1; }
cat "$state/journal" 2>/dev/null || true
exit 0
'''
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
        for name, body in (("busctl", BUSCTL), ("sudo", SUDO),
                           ("journalctl", JOURNALCTL), ("pw-dump", PW_DUMP)):
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

    def test_connect_verified_by_bluetooth_journal_even_when_pipewire_empty_or_broken(self) -> None:
        for pw_flag in ("pw-empty", "pw-broken"):
            with self.subTest(pw_flag=pw_flag):
                for p in ("pw-empty", "pw-broken", "hfp", "connected", "calls", "journal", "journalctl-args"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / pw_flag).touch()
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("HFP=up Connected=b true"), result.stdout)
                self.assertIn("source=journal", result.stdout)
                self.assertEqual(self.calls(), [f"ConnectProfile {HFP_AG}"])
                jargs = (self.stub / "journalctl-args").read_text()
                self.assertIn("-u bluetooth -o cat --since ", jargs)

    def test_connect_rejects_le_only_or_other_device_or_immediate_drop(self) -> None:
        for mode in ("le", "hfp-then-drop", "no-debug"):
            with self.subTest(mode=mode):
                for p in ("hfp", "connected", "calls", "journal"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "pw-empty").touch()
                # Another device's Hands-Free transition must never count for our phone.
                (self.stub / "journal").write_text(
                    f"src/service.c:change_state() 0xbbbb: device {OTHER_ADDRESS} "
                    "profile Hands-Free unit state changed: connecting -> connected (0)\n"
                )
                (self.stub / "connect").write_text(mode)
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 1)
                self.assertIn("no HFP profile", result.stderr)

    def test_connect_error_fails_with_error_name_only(self) -> None:
        (self.stub / "connect").write_text("fail")
        result = self.run_tool("connect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("org.bluez.Error.Failed", result.stderr)

    def test_status_reports_up_down_and_unknown_with_parseable_prefix(self) -> None:
        (self.stub / "pw-empty").touch()
        (self.stub / "connected").write_text("b true")
        (self.stub / "journal").write_text(
            f"src/service.c:change_state() 0x1: device {ADDRESS} "
            "profile Hands-Free unit state changed: connecting -> connected (0)\n"
        )
        res_up = self.run_tool("status")
        self.assertEqual(res_up.returncode, 0, res_up.stderr)
        self.assertTrue(res_up.stdout.startswith("HFP=up Connected=b true"), res_up.stdout)

        # Latest transition wins (connected -> disconnecting -> disconnected).
        with (self.stub / "journal").open("a") as fh:
            fh.write(
                f"src/service.c:change_state() 0x1: device {ADDRESS} "
                "profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n"
            )
        res_down = self.run_tool("status")
        self.assertEqual(res_down.returncode, 0, res_down.stderr)
        self.assertTrue(res_down.stdout.startswith("HFP=down Connected=b true"), res_down.stdout)

        # No change_state lines (bluetoothd not in -d mode) -> HFP=unknown with clear reason.
        (self.stub / "journal").write_text("bluetoothd[1]: Started\n")
        res_nodbg = self.run_tool("status")
        self.assertEqual(res_nodbg.returncode, 0, res_nodbg.stderr)
        self.assertTrue(res_nodbg.stdout.startswith("HFP=unknown"), res_nodbg.stdout)
        self.assertIn("no-bluetoothd-change-state-lines", res_nodbg.stdout)

        # Unreadable journal -> HFP=unknown, never down.
        (self.stub / "journal-unreadable").touch()
        res_unr = self.run_tool("status")
        self.assertEqual(res_unr.returncode, 0, res_unr.stderr)
        self.assertTrue(res_unr.stdout.startswith("HFP=unknown"), res_unr.stdout)
        self.assertIn("journal-unreadable", res_unr.stdout)

        # Device1.Connected=false proves down even if journal is unreadable.
        (self.stub / "connected").write_text("b false")
        res_disc = self.run_tool("status")
        self.assertEqual(res_disc.returncode, 0, res_disc.stderr)
        self.assertTrue(res_disc.stdout.startswith("HFP=down Connected=b false"), res_disc.stdout)

    def test_disconnect_verified_by_journal_or_bearer_down(self) -> None:
        for mode, expected_prefix in (("down-keep-link", "HFP=down Connected=b true"),
                                      ("down-bearer-only", "HFP=down Connected=b false")):
            with self.subTest(mode=mode):
                for p in ("hfp", "connected", "calls", "journal"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "pw-empty").touch()
                (self.stub / "connected").write_text("b true")
                (self.stub / "disconnect").write_text(mode)
                result = self.run_tool("disconnect")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith(expected_prefix), result.stdout)

    def test_disconnect_never_reports_down_when_journal_or_pipewire_unreadable(self) -> None:
        # Regression for P2: even if HFP was up and pw-dump breaks or journalctl fails,
        # disconnect must NEVER report HFP=down while Device1.Connected=true.
        for flag in ("pw-broken", "journal-unreadable"):
            with self.subTest(flag=flag):
                for p in ("pw-broken", "journal-unreadable", "journal"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "connected").write_text("b true")
                (self.stub / "hfp").touch()
                (self.stub / flag).touch()
                (self.stub / "disconnect").write_text("stuck-keep-link")
                result = self.run_tool("disconnect")
                self.assertEqual(result.returncode, 1)
                self.assertIn("unverifiable", result.stderr)

    def test_disconnect_that_leaves_hfp_connected_fails(self) -> None:
        (self.stub / "connected").write_text("b true")
        (self.stub / "disconnect").write_text("stuck-connected")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("HFP still active", result.stderr)

    def test_failed_disconnect_exits_nonzero(self) -> None:
        (self.stub / "connected").write_text("b true")
        (self.stub / "disconnect").write_text("fail")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("DisconnectProfile(HFP AG) failed", result.stderr)

    def test_already_down_is_success(self) -> None:
        (self.stub / "disconnect").write_text("notconnected")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("HFP=down"), result.stdout)

    def test_only_profile_scoped_calls(self) -> None:
        self.run_tool("connect")
        self.run_tool("disconnect")
        for call in self.calls():
            self.assertRegex(call, r"^(Connect|Disconnect)Profile 0000111f-")
            self.assertNotRegex(call, r"RemoveDevice|CancelPairing|^Connect |^Disconnect ")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
