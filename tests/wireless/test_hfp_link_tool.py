#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""#160/#179: the test-only HFP operator tool verifies HFP via bluetoothd journal cursors.

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
    hfp-bearer-down)
      echo 'b false' > "$state/connected"
      printf 'src/service.c:change_state() 0xaaaa1111: device %s profile Hands-Free unit state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
      ;;
    gateway-only)
      echo 'b true' > "$state/connected"
      printf 'src/service.c:change_state() 0xaaaa3333: device %s profile Hands-Free Voice gateway state changed: connecting -> connected (0)\n' "$AA_STUB_ADDRESS" >> "$state/journal"
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
# Stubs real journalctl cursor semantics:
#   - `--show-cursor` returns `-- cursor: line=<current line count of $state/journal>`
#   - `--after-cursor line=<N>` emits ONLY lines strictly after line N (so same-second
#     transitions written after the cursor are returned, and stale lines before the cursor
#     are never returned).
JOURNALCTL = r'''#!/usr/bin/env bash
state=$AA_STUB_DIR
echo "$*" >> "$state/journalctl-args"
[ -f "$state/journal-unreadable" ] && { echo "No journal files were opened due to insufficient permissions." >&2; exit 1; }
for arg in "$@"; do
  if [ "$arg" = "--show-cursor" ]; then
    [ -f "$state/journal-no-cursor" ] && exit 0
    lines=0
    [ -f "$state/journal" ] && lines=$(wc -l < "$state/journal")
    printf -- '-- cursor: line=%s\n' "$lines"
    exit 0
  fi
done
after=""
prev=""
for arg in "$@"; do
  if [ "$prev" = "--after-cursor" ]; then after=$arg; fi
  prev=$arg
done
if [ -n "$after" ]; then
  n=${after#line=}
  if [ -f "$state/journal" ]; then
    tail -n +"$((n + 1))" "$state/journal"
  fi
  exit 0
fi
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

    def test_connect_verified_by_cursor_in_same_second_even_when_pipewire_empty_or_broken(self) -> None:
        for pw_flag in ("pw-empty", "pw-broken"):
            with self.subTest(pw_flag=pw_flag):
                for p in ("pw-empty", "pw-broken", "hfp", "connected", "calls", "journal", "journalctl-args"):
                    (self.stub / p).unlink(missing_ok=True)
                # Pre-existing stale disconnected line before cursor; connect transition lands
                # immediately (same second) after the cursor.
                (self.stub / "journal").write_text(
                    f"src/service.c:change_state() 0x0: device {ADDRESS} "
                    "profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n"
                )
                (self.stub / pw_flag).touch()
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("HFP=up Connected=b true"), result.stdout)
                self.assertIn("source=journal", result.stdout)
                self.assertEqual(self.calls(), [f"ConnectProfile {HFP_AG}"])
                jargs = (self.stub / "journalctl-args").read_text()
                self.assertIn("-u bluetooth -n 0 --show-cursor -o cat", jargs)
                self.assertIn("-u bluetooth -o cat --after-cursor line=1", jargs)
                self.assertNotIn("--since", jargs)

    def test_connect_fails_unknown_when_cursor_unavailable(self) -> None:
        for flag in ("journal-unreadable", "journal-no-cursor"):
            with self.subTest(flag=flag):
                for p in ("journal-unreadable", "journal-no-cursor", "calls"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / flag).touch()
                result = self.run_tool("connect")
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stdout.startswith("HFP=unknown"), result.stdout)
                self.assertIn("cursor", result.stderr)
                self.assertEqual(self.calls(), [])

    def test_connect_rejects_le_only_or_other_device_or_immediate_drop_or_stale_connected(self) -> None:
        for mode in ("le", "hfp-then-drop", "no-debug", "gateway-only", "hfp-bearer-down"):
            with self.subTest(mode=mode):
                for p in ("hfp", "connected", "calls", "journal"):
                    (self.stub / p).unlink(missing_ok=True)
                (self.stub / "pw-empty").touch()
                # Pre-cursor connected line for this device AND another device must not count.
                (self.stub / "journal").write_text(
                    f"src/service.c:change_state() 0xaaaa: device {ADDRESS} "
                    "profile Hands-Free unit state changed: connecting -> connected (0)\n"
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

        with (self.stub / "journal").open("a") as fh:
            fh.write(
                f"src/service.c:change_state() 0x1: device {ADDRESS} "
                "profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n"
            )
        res_down = self.run_tool("status")
        self.assertEqual(res_down.returncode, 0, res_down.stderr)
        self.assertTrue(res_down.stdout.startswith("HFP=down Connected=b true"), res_down.stdout)

        (self.stub / "journal").write_text("bluetoothd[1]: Started\n")
        res_nodbg = self.run_tool("status")
        self.assertEqual(res_nodbg.returncode, 0, res_nodbg.stderr)
        self.assertTrue(res_nodbg.stdout.startswith("HFP=unknown"), res_nodbg.stdout)
        self.assertIn("no-bluetoothd-change-state-lines", res_nodbg.stdout)

        (self.stub / "journal-unreadable").touch()
        res_unr = self.run_tool("status")
        self.assertEqual(res_unr.returncode, 0, res_unr.stderr)
        self.assertTrue(res_unr.stdout.startswith("HFP=unknown"), res_unr.stdout)
        self.assertIn("journal-unreadable", res_unr.stdout)

        (self.stub / "connected").write_text("b false")
        res_disc = self.run_tool("status")
        self.assertEqual(res_disc.returncode, 0, res_disc.stderr)
        self.assertTrue(res_disc.stdout.startswith("HFP=down Connected=b false"), res_disc.stdout)

    def test_disconnect_verified_by_post_cursor_journal_or_bearer_down(self) -> None:
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

    def test_disconnect_ignores_stale_disconnected_transition_before_cursor(self) -> None:
        # P3 regression: a stale `-> disconnected` transition before the cursor must NEVER
        # satisfy DisconnectProfile while Device1.Connected is still true.
        (self.stub / "connected").write_text("b true")
        (self.stub / "pw-empty").touch()
        (self.stub / "journal").write_text(
            f"src/service.c:change_state() 0x1: device {ADDRESS} "
            "profile Hands-Free unit state changed: disconnecting -> disconnected (0)\n"
        )
        (self.stub / "disconnect").write_text("stuck-keep-link")
        result = self.run_tool("disconnect")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unverifiable", result.stderr)

    def test_disconnect_never_reports_down_when_journal_or_pipewire_unreadable(self) -> None:
        for flag in ("pw-broken", "journal-unreadable", "journal-no-cursor"):
            with self.subTest(flag=flag):
                for p in ("pw-broken", "journal-unreadable", "journal-no-cursor", "journal"):
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
