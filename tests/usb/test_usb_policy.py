# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/usb/test_usb_policy.py [rules|rollback|udevadm]
"""Host-side USB policy fixtures: no host udev state is ever modified.

Covers the task-29 udev policy boundaries:
- structural matching of 70-android-auto-receiver.rules (qualified ids + port
  path match; same ids on another port, HID dongles, usb-serial devices and
  hubs never match),
- fail-closed handling of malformed rule text (parser AND `udevadm verify`),
- install/rollback state machine in a fake root (idempotent, exact restore),
- parsing of recorded `udevadm test` output (matched vs unmatched evidence).

The real `udevadm test` runs live on the target; the recorded fixtures freeze
what those runs proved (see .omo/evidence ... task-29-usb-policy.json).
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/usb-policy"))
import rules_check  # noqa: E402

RULES_FILE = ROOT / "tools/usb-policy/70-android-auto-receiver.rules"
POLICY_DIR = ROOT / "tools/usb-policy"
FIXTURES = ROOT / "tests/usb/fixtures/usb_policy"
MM_IGNORE = "ENV{ID_MM_DEVICE_IGNORE}"

# Qualified identities (the only things the rules may ever match on).
PHONE = ("04e8", "6860")
AOA = ("18d1", "2d00")
AOA_ADB = ("18d1", "2d01")
PORT = "1-2.1"
# Known-unmatched devices on the qualified target (failure QA).
DONGLE = ("3938", "1192")
DONGLE_PORT = "1-2.4"
HUB = ("0bda", "5489")
BT = ("13d3", "3549")
USB_SERIAL = ("067b", "2303")


def usb_device(kernel: str, vendor: str, product: str, **env: str) -> rules_check.Device:
    properties = {"DEVTYPE": "usb_device", **env}
    return rules_check.Device(
        kernel=kernel,
        subsystem="usb",
        attrs={"idVendor": vendor, "idProduct": product},
        env=properties,
    )


def child_of(parent: rules_check.Device, kernel: str, subsystem: str,
             **env: str) -> rules_check.Device:
    return rules_check.Device(kernel=kernel, subsystem=subsystem, env=dict(env),
                              parents=(parent,))


def parse_real_rules() -> list[rules_check.Rule]:
    return rules_check.parse_rules(RULES_FILE.read_text(encoding="utf-8"))


class Rules(unittest.TestCase):
    """Structural matching of the shipped rules file (no udevadm needed)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.rules = parse_real_rules()

    def apply(self, device: rules_check.Device) -> dict[str, str]:
        return rules_check.apply_rules(self.rules, device)

    def test_rules_file_parses_exactly_six_rules(self) -> None:
        self.assertEqual(len(self.rules), 6)
        for rule in self.rules:
            self.assertTrue(rule.assignments, f"rule {rule.line_no} assigns nothing")

    def test_phone_mode_matched_exactly(self) -> None:
        result = self.apply(usb_device(PORT, *PHONE))
        self.assertEqual(result[MM_IGNORE], "1")
        self.assertEqual(result["GROUP"], "plugdev")
        self.assertEqual(result["MODE"], "0660")
        self.assertEqual(result["TAG"], "uaccess")

    def test_aoa_accessory_ids_matched_exactly(self) -> None:
        for vendor, product in (AOA, AOA_ADB):
            with self.subTest(id=f"{vendor}:{product}"):
                result = self.apply(usb_device(PORT, vendor, product))
                self.assertEqual(result[MM_IGNORE], "1")
                self.assertEqual(result["GROUP"], "plugdev")
                self.assertEqual(result["MODE"], "0660")
                self.assertEqual(result["TAG"], "uaccess")

    def test_same_ids_on_other_ports_never_match(self) -> None:
        # The reviewer's lever: identical ids on a different port path.
        for kernel in ("1-2.2", "1-2.4", "1-3", "2-1.1", "1-2", "1-2.10", "1-2.11"):
            for vendor, product in (PHONE, AOA, AOA_ADB):
                with self.subTest(port=kernel, id=f"{vendor}:{product}"):
                    self.assertEqual(self.apply(usb_device(kernel, vendor, product)), {})

    def test_dongle_never_matches(self) -> None:
        device = usb_device(DONGLE_PORT, *DONGLE)
        self.assertEqual(self.apply(device), {})
        self.assertEqual(self.apply(child_of(device, "hidraw0", "hidraw")), {})

    def test_unmatched_usb_serial_never_matches(self) -> None:
        device = usb_device("1-2.3", *USB_SERIAL)
        self.assertEqual(self.apply(device), {})
        tty = child_of(device, "ttyUSB0", "tty", DEVTYPE="tty")
        self.assertEqual(self.apply(tty), {})

    def test_hubs_bluetooth_and_root_hubs_never_match(self) -> None:
        for kernel, ids in (("1-2", HUB), ("2-1", ("0bda", "0489")), ("1-3", BT),
                            ("usb1", ("1d6b", "0002")), ("usb2", ("1d6b", "0003"))):
            with self.subTest(kernel=kernel):
                self.assertEqual(self.apply(usb_device(kernel, *ids)), {})

    def test_tty_child_gets_mm_ignore_but_no_acl(self) -> None:
        # Least privilege: the chain rule covers tty ports for ModemManager,
        # but GROUP/MODE/TAG (device-node ACL) must never reach children.
        phone = usb_device(PORT, *PHONE)
        interface = child_of(phone, "1-2.1:1.1", "usb", DEVTYPE="usb_interface")
        tty = child_of(interface, "ttyACM0", "tty")
        result = self.apply(tty)
        self.assertEqual(result, {MM_IGNORE: "1"})
        self.assertNotIn("GROUP", result)
        self.assertNotIn("MODE", result)
        self.assertNotIn("TAG", result)
        self.assertEqual(self.apply(interface), {MM_IGNORE: "1"})

    def test_untrusted_device_text_is_never_consulted(self) -> None:
        # untrusted_external_text: hostile strings in device-supplied fields
        # cannot influence matching (only ids/port/subsystem are matched).
        hostile_attrs = {
            "idVendor": PHONE[0],
            "idProduct": PHONE[1],
            "manufacturer": 'X", MODE="0666", KERNEL=="*',
            "product": "ID_MM_DEVICE_IGNORE=1",
            "serial": "TEST-SERIAL-001; rm -rf /",
        }
        benign = rules_check.Device(kernel=PORT, subsystem="usb", attrs=hostile_attrs,
                                    env={"DEVTYPE": "usb_device"})
        self.assertEqual(self.apply(benign), self.apply(usb_device(PORT, *PHONE)))
        # ...and a different serial on the matched identity changes nothing.
        other_attrs = dict(hostile_attrs, serial="OTHER-PSEUDONYM")
        other = rules_check.Device(kernel=PORT, subsystem="usb", attrs=other_attrs,
                                   env={"DEVTYPE": "usb_device"})
        self.assertEqual(self.apply(other), self.apply(usb_device(PORT, *PHONE)))

    def test_malformed_lines_rejected(self) -> None:
        samples = [
            'SUBSYSTEM=="usb" ATTR{idVendor}=="04e8"',
            'KERNEL=="1-2.1", ATTR{idVendor}=="04e8',
            'NONSENSEKEY=="usb", MODE="0666"',
            "SUBSYSTEM==\"usb\", MODE",
            ', MODE="0666"',
            "KERNEL==\"1-2.1\", GARBAGE",
            'SUBSYSTEM=="usb", KERNEL=="1-2.1",',
            'MODE="0666"',                       # assignment only
            'KERNEL=="1-2.1"',                   # match only
            "totally not a rule",
        ]
        for sample in samples:
            with self.subTest(sample=sample):
                with self.assertRaises(rules_check.RuleError):
                    rules_check.parse_rules(sample)

    def test_malformed_file_rejected_not_matched_everything(self) -> None:
        broken = (FIXTURES / "malformed.rules").read_text(encoding="utf-8")
        with self.assertRaises(rules_check.RuleError):
            rules_check.parse_rules(broken)

    def test_policy_gate_accepts_shipped_rules(self) -> None:
        rules_check.verify_exact_policy(self.rules)
        run = subprocess.run(
            ["python3", str(POLICY_DIR / "rules_check.py"), "verify", str(RULES_FILE)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("exact approved policy", run.stdout)

    def test_policy_gate_rejects_hostile_all_match_rule(self) -> None:
        # Gate-proof finding F2: this single line parsed clean ("ok: 1 rules",
        # exit 0) before the gate existed; as root it would grant world device
        # access anywhere. Syntax validity must never pass the gate.
        hostile = rules_check.parse_rules(
            'KERNEL=="*", MODE:="0666", ENV{ID_MM_DEVICE_IGNORE}="1"')
        self.assertEqual(len(hostile), 1)
        with self.assertRaises(rules_check.PolicyError):
            rules_check.verify_exact_policy(hostile)

    def test_policy_gate_rejects_appended_rule(self) -> None:
        text = RULES_FILE.read_text(encoding="utf-8") + '\nKERNEL=="*", MODE:="0666"\n'
        with self.assertRaises(rules_check.PolicyError):
            rules_check.verify_exact_policy(rules_check.parse_rules(text))

    def test_verify_requires_byte_identical_policy(self) -> None:
        approved = RULES_FILE.read_bytes()
        for change in (b'\nKERNEL=="*", MODE:="0666"\n', b"\n# comment-only change\n"):
            with self.subTest(change=change):
                # Given
                with tempfile.TemporaryDirectory(prefix="usb-policy-verify-") as tmp:
                    candidate = Path(tmp) / RULES_FILE.name
                    candidate.write_bytes(approved + change)
                    # When
                    run = subprocess.run(
                        ["python3", str(POLICY_DIR / "rules_check.py"), "verify", str(candidate)],
                        capture_output=True, text=True, timeout=30)
                    # Then
                    self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)


    def test_policy_gate_rejects_weakened_dropped_and_reordered_rules(self) -> None:
        lines = [ln for ln in RULES_FILE.read_text(encoding="utf-8").splitlines()
                 if ln.strip() and not ln.startswith("#")]
        self.assertEqual(len(lines), 6)
        weakened = [ln.replace('MODE="0660"', 'MODE="0666"') if i == 3 else ln
                    for i, ln in enumerate(lines)]
        dropped = lines[:5]
        reordered = [lines[1], lines[0]] + lines[2:]
        for name, candidate in (("weakened", weakened), ("dropped", dropped),
                                ("reordered", reordered)):
            with self.subTest(case=name):
                with self.assertRaises(rules_check.PolicyError):
                    rules_check.verify_exact_policy(
                        rules_check.parse_rules("\n".join(candidate) + "\n"))

    def test_udevadm_verify_accepts_shipped_rules(self) -> None:
        if shutil.which("udevadm") is None:
            self.skipTest("udevadm not installed on this host")
        probe = subprocess.run(["udevadm", "verify", "--help"], capture_output=True, timeout=30)
        if probe.returncode != 0:
            self.skipTest("udevadm verify unavailable (systemd < 255)")
        run = subprocess.run(
            ["udevadm", "verify", "--no-style", "-N", "never", str(RULES_FILE)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_udevadm_verify_rejects_malformed(self) -> None:
        if shutil.which("udevadm") is None:
            self.skipTest("udevadm not installed on this host")
        probe = subprocess.run(["udevadm", "verify", "--help"], capture_output=True, timeout=30)
        if probe.returncode != 0:
            self.skipTest("udevadm verify unavailable (systemd < 255)")
        broken = FIXTURES / "malformed.rules"
        run = subprocess.run(
            ["udevadm", "verify", "--no-style", "-N", "never", str(broken)],
            capture_output=True, text=True, timeout=60)
        self.assertNotEqual(run.returncode, 0, "udevadm must reject malformed rules")


class Rollback(unittest.TestCase):
    """install.sh / rollback.sh state machine in a fake root (AA_USB_POLICY_ROOT)."""

    def setUp(self) -> None:
        # Fixture dirs must look like the live /etc/udev / /var/lib chain:
        # 0755, never group-writable (umask 002 hosts would otherwise create
        # 0775 and trip the installer's trust checks).
        previous_umask = os.umask(0o022)
        self.addCleanup(os.umask, previous_umask)
        self.tmp = tempfile.TemporaryDirectory(prefix="usb-policy-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, AA_USB_POLICY_ROOT=str(self.root))
        self.dst = self.root / "etc/udev/rules.d/70-android-auto-receiver.rules"
        self.state = self.root / "var/lib/android-auto-receiver/usb-policy"

    def run_script(self, name: str, env: dict[str, str] | None = None):
        return subprocess.run(
            ["bash", str(POLICY_DIR / name)],
            env=env or self.env, capture_output=True, text=True, timeout=60)

    def install(self):
        return self.run_script("install.sh")

    def rollback(self):
        return self.run_script("rollback.sh")

    def test_install_writes_rules_and_manifest(self) -> None:
        run = self.install()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(self.dst.is_file())
        self.assertEqual(self.dst.read_bytes(), RULES_FILE.read_bytes())
        manifest = (self.state / "manifest").read_text(encoding="utf-8")
        self.assertIn("schema=aa-usb-policy-state/1", manifest)
        self.assertIn("prior_existed=0", manifest)
        self.assertIn("installed_sha256=", manifest)
        self.assertFalse((self.state / "prior.rules").exists())

    def test_scripts_reject_fake_roots_resolving_to_live_root(self) -> None:
        # Given: stop BEFORE destination construction even on vulnerable code;
        # the regression must never risk writes to the real /etc or /var/lib.
        guard = self.root / "guard.bash"
        guard.write_text(
            "trap 'case \"$BASH_COMMAND\" in DST=*) "
            "printf \"destination construction reached before rejection\\n\" >&2; "
            "exit 97;; esac' DEBUG\n", encoding="utf-8")
        link = self.root / "live-root"
        link.symlink_to("/", target_is_directory=True)
        aliases = ("/", "//", "///", "/./", "//./", "/../",
                   str(link), f"{link}/", f"{link}//", f"{link}/./",
                   str(self.root) + "/.." * (len(self.root.parts) - 1))
        for script in ("install.sh", "rollback.sh"):
            for alias in aliases:
                with self.subTest(script=script, root=alias):
                    env = dict(self.env, AA_USB_POLICY_ROOT=alias, BASH_ENV=str(guard))
                    # When
                    run = self.run_script(script, env)
                    # Then
                    self.assertEqual(run.returncode, 1,
                                     "live-root alias must be rejected before paths: " + run.stderr)
                    self.assertIn("live filesystem root", run.stderr)
                    self.assertNotIn("udev-skip", run.stdout)

    def test_scripts_accept_equivalent_safe_fake_roots(self) -> None:
        for suffix in ("/", "/./", "//"):
            with self.subTest(suffix=suffix):
                # Given
                env = dict(self.env, AA_USB_POLICY_ROOT=str(self.root) + suffix)
                # When
                installed = self.run_script("install.sh", env)
                rolled_back = self.run_script("rollback.sh", env)
                # Then
                self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
                self.assertIn("udev-skip", installed.stdout)
                self.assertEqual(rolled_back.returncode, 0,
                                 rolled_back.stdout + rolled_back.stderr)
                self.assertFalse(self.dst.exists())
                self.assertFalse(self.state.exists())

    def test_install_twice_is_idempotent(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        manifest_first = (self.state / "manifest").read_bytes()
        run = self.install()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("already-installed", run.stdout)
        self.assertEqual((self.state / "manifest").read_bytes(), manifest_first)
        self.assertEqual(self.dst.read_bytes(), RULES_FILE.read_bytes())

    def test_install_refuses_mode_drift_when_bytes_unchanged(self) -> None:
        for mode in (0o666, 0o664, 0o600, 0o4644):
            with self.subTest(mode=oct(mode)):
                # Given
                self.assertEqual(self.install().returncode, 0)
                manifest = (self.state / "manifest").read_bytes()
                self.dst.chmod(mode)
                self.addCleanup(self.dst.chmod, 0o644)
                # When
                run = self.install()
                # Then
                self.assertEqual(run.returncode, 1,
                                 "unsafe mode must not report success: " + run.stdout + run.stderr)
                self.assertNotIn("already-installed", run.stdout)
                self.assertEqual((self.state / "manifest").read_bytes(), manifest)
                self.assertEqual(self.dst.stat().st_mode & 0o7777, mode)
                self.dst.chmod(0o644)

    @unittest.skipUnless(shutil.which("fakeroot"), "fakeroot required for foreign uid fixture")
    def test_install_refuses_owner_drift_when_bytes_unchanged(self) -> None:
        # Given: fakeroot supplies real stat/chown semantics without host privileges.
        self.assertEqual(self.install().returncode, 0)
        manifest = (self.state / "manifest").read_bytes()
        # When
        run = subprocess.run(
            ["fakeroot", "bash", "-c",
             'set -euo pipefail; chown 12345 "$1"; bash "$2"',
             "metadata-test", str(self.dst), str(POLICY_DIR / "install.sh")],
            env=self.env, capture_output=True, text=True, timeout=60)
        # Then
        self.assertEqual(run.returncode, 1,
                         "unsafe owner must not report success: " + run.stdout + run.stderr)
        self.assertNotIn("already-installed", run.stdout)
        self.assertEqual((self.state / "manifest").read_bytes(), manifest)

    def test_install_accepts_safe_metadata_when_bytes_unchanged(self) -> None:
        # Given
        self.assertEqual(self.install().returncode, 0)
        original = self.dst.stat()
        self.assertEqual(original.st_uid, os.geteuid())
        self.assertEqual(original.st_mode & 0o7777, 0o644)
        # When
        run = self.install()
        # Then
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("already-installed", run.stdout)
        self.assertEqual(self.dst.stat().st_ino, original.st_ino)

    def test_rollback_restores_absent_state_exactly(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertFalse(self.dst.exists())
        self.assertFalse(self.state.exists())
        self.assertFalse((self.root / "var/lib/android-auto-receiver").exists())
        self.assertEqual(sorted((self.root / "etc/udev/rules.d").glob("*")), [])

    def test_rollback_restores_prior_content_byte_exact(self) -> None:
        self.dst.parent.mkdir(parents=True)
        prior_bytes = b"# prior local policy\nKERNEL==\"9-9.9\", MODE=\"0600\"\n"
        self.dst.write_bytes(prior_bytes)
        self.assertEqual(self.install().returncode, 0)
        self.assertTrue((self.state / "prior.rules").is_file())
        self.assertEqual(self.install().returncode, 0)  # second install keeps prior record
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), prior_bytes)
        self.assertFalse(self.state.exists())

    def test_rollback_twice_is_idempotent(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        self.assertEqual(self.rollback().returncode, 0)
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("nothing-to-rollback", run.stdout)
        self.assertFalse(self.dst.exists())

    def test_rollback_without_manifest_is_noop(self) -> None:
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("nothing-to-rollback", run.stdout)

    def test_rollback_refuses_corrupt_manifest(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        (self.state / "manifest").write_text("schema=unknown/9\n", encoding="utf-8")
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertTrue(self.dst.exists(), "corrupt manifest must not cause deletions")

    def test_rollback_refuses_tampered_backup(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# prior\n")
        self.assertEqual(self.install().returncode, 0)
        (self.state / "prior.rules").write_bytes(b"# tampered\n")
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertTrue(self.dst.exists(), "tampered backup must not be installed")
        self.assertTrue((self.state / "manifest").exists())

    def edit_manifest(self, **fields: str | None) -> None:
        """Rewrite manifest keys in place; a None value removes the key."""
        manifest = self.state / "manifest"
        out: list[str] = []
        seen: set[str] = set()
        for line in manifest.read_text(encoding="utf-8").splitlines():
            key = line.split("=", 1)[0]
            if key in fields:
                seen.add(key)
                if fields[key] is not None:
                    out.append(f"{key}={fields[key]}")
            else:
                out.append(line)
        for key, value in fields.items():
            if key not in seen and value is not None:
                out.append(f"{key}={value}")
        manifest.write_text("\n".join(out) + "\n", encoding="utf-8")

    def assert_repair_refused(self, edited: bytes) -> None:
        self.dst.write_bytes(edited)
        manifest_before = (self.state / "manifest").read_bytes()
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.dst.read_bytes(), edited,
                         "installed policy must not be modified on corrupt saved state")
        self.assertEqual((self.state / "manifest").read_bytes(), manifest_before,
                         "corrupt saved state must not be rewritten")

    def test_install_refuses_symlinked_destination(self) -> None:
        self.dst.parent.mkdir(parents=True)
        target = self.root / "innocent-bystander.rules"
        target.write_bytes(b"# innocent bystander\n")
        target.chmod(0o600)
        self.dst.symlink_to(target)
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertEqual(target.read_bytes(), b"# innocent bystander\n",
                         "cp must not write through a symlinked destination")
        self.assertEqual(target.stat().st_mode & 0o777, 0o600,
                         "chmod must not follow a symlinked destination")
        self.assertTrue(self.dst.is_symlink())
        self.assertEqual(os.readlink(self.dst), str(target))
        self.assertFalse(self.state.exists(), "no manifest may be written")

    def test_install_refuses_dangling_symlink_destination(self) -> None:
        self.dst.parent.mkdir(parents=True)
        missing = self.root / "not-here.rules"
        self.dst.symlink_to(missing)
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertTrue(self.dst.is_symlink(), "dangling link must survive untouched")
        self.assertEqual(os.readlink(self.dst), str(missing))
        self.assertFalse(missing.exists())
        self.assertFalse(self.state.exists(), "no manifest may be written")

    def test_install_never_writes_or_chmods_through_null_mask_symlink(self) -> None:
        # The legitimate udev mask is a symlink to /dev/null; install must
        # refuse and must never chmod through the link (which would re-mode
        # /dev/null itself). A fake-root stand-in asserts the same on content,
        # portably regardless of the test user's privileges.
        self.dst.parent.mkdir(parents=True)
        null_mode_before = os.stat("/dev/null").st_mode & 0o777
        self.dst.symlink_to("/dev/null")
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertEqual(os.stat("/dev/null").st_mode & 0o777, null_mode_before,
                         "/dev/null permissions must never change")
        self.assertFalse(self.state.exists(), "no manifest may be written")
        self.dst.unlink()

        standin = self.root / "dev/null-standin"
        standin.parent.mkdir(parents=True)
        standin.write_bytes(b"")
        standin.chmod(0o666)
        self.dst.symlink_to(standin)
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertEqual(standin.read_bytes(), b"", "no bytes may be written through the link")
        self.assertEqual(standin.stat().st_mode & 0o777, 0o666,
                         "chmod must not follow the link")
        self.assertTrue(self.dst.is_symlink())
        self.assertFalse(self.state.exists(), "no manifest may be written")

    def test_rollback_refuses_drifted_installed_rule(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        edited = b"# someone else's edit\n"
        self.dst.write_bytes(edited)
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("drifted", run.stderr)
        self.assertEqual(self.dst.read_bytes(), edited, "edited file must survive rollback")
        self.assertTrue((self.state / "manifest").exists(), "state must stay untouched")

    def test_rollback_refuses_drifted_installed_rule_with_prior(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# prior\n")
        self.assertEqual(self.install().returncode, 0)
        edited = b"# someone else's edit\n"
        self.dst.write_bytes(edited)
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("drifted", run.stderr)
        self.assertEqual(self.dst.read_bytes(), edited,
                         "edited file must not be overwritten by the restore")
        self.assertTrue((self.state / "prior.rules").exists())
        self.assertTrue((self.state / "manifest").exists())

    def test_install_refuses_manifest_bad_prior_existed(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_existed="7")
        self.assert_repair_refused(b"# post-install drift\n")

    def test_install_refuses_manifest_missing_prior_sha256(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_sha256=None)
        self.assert_repair_refused(b"# post-install drift\n")

    def test_install_refuses_manifest_missing_backup(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# prior\n")
        self.assertEqual(self.install().returncode, 0)
        self.assertTrue((self.state / "prior.rules").is_file())
        (self.state / "prior.rules").unlink()
        self.assert_repair_refused(b"# post-install drift\n")
        self.assertFalse((self.state / "prior.rules").exists())

    def test_install_refuses_manifest_backup_hash_mismatch(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# prior\n")
        self.assertEqual(self.install().returncode, 0)
        (self.state / "prior.rules").write_bytes(b"# tampered backup\n")
        self.assert_repair_refused(b"# post-install drift\n")
        self.assertEqual((self.state / "prior.rules").read_bytes(), b"# tampered backup\n")

    def test_install_refuses_corrupt_manifest_before_claiming_success(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_existed="nope")
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertNotIn("already-installed", run.stdout,
                         "corrupt saved state must not be claimed as a clean install")

    def test_rollback_refuses_prior_existed_flip(self) -> None:
        # Finding F4: flipping only prior_existed=1->0 must never let rollback
        # DELETE the original backup (it would drop the only restore point).
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_existed="0", prior_sha256="-")
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual((self.state / "prior.rules").read_bytes(), b"# original prior\n",
                         "the original backup must survive a flipped manifest")
        self.assertEqual(self.dst.read_bytes(), RULES_FILE.read_bytes(),
                         "the installed policy must be untouched")
        self.assertTrue((self.state / "manifest").exists())

    def test_install_refuses_prior_existed_flip(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_existed="0", prior_sha256="-")
        self.assert_repair_refused(b"# post-install drift\n")
        self.assertEqual((self.state / "prior.rules").read_bytes(), b"# original prior\n")

    def test_rollback_refuses_prior_existed_flip_reverse(self) -> None:
        # prior_existed=0->1 with no backup on disk is equally inconsistent.
        self.assertEqual(self.install().returncode, 0)
        self.edit_manifest(prior_existed="1",
                           prior_sha256="0" * 64)
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertTrue(self.dst.exists(), "no deletion on inconsistent state")
        self.assertTrue((self.state / "manifest").exists())

    def test_install_refuses_surviving_backup_without_manifest(self) -> None:
        # Finding F4: a missing manifest with a surviving backup must NOT be
        # treated as a fresh install (that silently re-bases the backup onto
        # the already-installed policy and loses the original).
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        self.assertEqual(self.install().returncode, 0)
        (self.state / "manifest").unlink()
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual((self.state / "prior.rules").read_bytes(), b"# original prior\n",
                         "the original backup must not be re-based onto the installed policy")

    def test_rollback_refuses_surviving_backup_without_manifest(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        self.assertEqual(self.install().returncode, 0)
        (self.state / "manifest").unlink()
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("without a manifest", run.stderr)
        self.assertEqual((self.state / "prior.rules").read_bytes(), b"# original prior\n")
        self.assertTrue(self.dst.exists())

    def test_manifest_duplicate_and_unknown_keys_refused(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        manifest = self.state / "manifest"
        original = manifest.read_text(encoding="utf-8")
        manifest.write_text(original + "prior_existed=1\n", encoding="utf-8")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertNotEqual(self.rollback().returncode, 0)
        manifest.write_text(original + "evil_key=1\n", encoding="utf-8")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertNotEqual(self.rollback().returncode, 0)
        manifest.write_text(original, encoding="utf-8")
        self.assertEqual(self.install().returncode, 0, "restored manifest must validate again")

    def test_install_interrupt_after_dst_publish_recovers_baseline(self) -> None:
        # Finding F5 fault injection: kill install after the policy publish
        # and before the phase=installed record. Rollback must still end with
        # the ORIGINAL baseline intact - the installed policy must never be
        # taken for the prior state.
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-publish-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD="exit 137")
        run = self.run_script("install.sh", env)
        self.assertEqual(run.returncode, 137, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), RULES_FILE.read_bytes())
        manifest = (self.state / "manifest").read_text(encoding="utf-8")
        self.assertIn("phase=installing", manifest,
                      "the interrupted publish must leave the recoverable phase record")
        self.assertIn(f"prior_sha256={hashlib.sha256(b'# original prior' + chr(10).encode()).hexdigest()}",
                      manifest, "the prior record must still name the ORIGINAL baseline")
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n",
                         "rollback must restore the ORIGINAL baseline, not the installed policy")
        self.assertFalse(self.state.exists())

    def test_install_interrupt_before_dst_publish_recovers_baseline(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-stage-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD="exit 137")
        run = self.run_script("install.sh", env)
        self.assertEqual(run.returncode, 137, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n",
                         "the interrupted publish must not have touched the destination")
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n")
        self.assertFalse(self.state.exists())

    def test_install_interrupt_then_rerun_keeps_prior_record(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-publish-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD="exit 137")
        self.assertNotEqual(self.run_script("install.sh", env).returncode, 0)
        run = self.install()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        prior_sha = hashlib.sha256(b"# original prior\n").hexdigest()
        manifest = (self.state / "manifest").read_text(encoding="utf-8")
        self.assertIn(f"prior_sha256={prior_sha}", manifest,
                      "a rerun must never re-base the prior record onto the installed policy")
        self.assertIn("phase=installed", manifest)
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n")

    def test_rollback_reload_failure_then_rerun_completes(self) -> None:
        # Finding F5: a udev reload failure after the restore must not strand
        # the rollback (the old flow refused the rerun on hash mismatch). The
        # phase record lets a rerun finish with the baseline intact.
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        self.assertEqual(self.install().returncode, 0)
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="before-reload",
                   AA_USB_POLICY_TEST_HOOK_CMD="exit 1")
        run = self.run_script("rollback.sh", env)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n",
                         "the restore itself must have completed before the reload failure")
        self.assertIn("phase=restored", (self.state / "manifest").read_text(encoding="utf-8"))
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n")
        self.assertFalse(self.state.exists(), "the rerun must finish the cleanup")

    def test_install_and_rollback_serialize_on_state_lock(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# original prior\n")
        ready = self.root / "lock-ready"
        release = self.root / "lock-release"
        os.mkfifo(ready)
        os.mkfifo(release)
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-check-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD=f'printf ready >"{ready}"; read -r <"{release}"')
        holder = subprocess.Popen(["bash", str(POLICY_DIR / "install.sh")], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with ready.open(encoding="utf-8") as signal:
                self.assertEqual(signal.read(), "ready")
            run = self.install()
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("in progress", run.stderr)
            run = self.rollback()
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("in progress", run.stderr)
        finally:
            with release.open("w", encoding="utf-8") as signal:
                signal.write("release\n")
            holder.communicate(timeout=30)
        self.assertEqual(holder.returncode, 0)
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(self.dst.read_bytes(), b"# original prior\n",
                         "the serialized install must complete and roll back cleanly")

    def test_install_refuses_malformed_rules(self) -> None:
        # Fail closed: validation happens before ANY system state change.
        with tempfile.TemporaryDirectory(prefix="usb-policy-broken-") as tmp:
            copy_dir = Path(tmp) / "usb-policy"
            shutil.copytree(POLICY_DIR, copy_dir)
            (copy_dir / "70-android-auto-receiver.rules").write_text(
                (FIXTURES / "malformed.rules").read_text(encoding="utf-8"), encoding="utf-8")
            run = subprocess.run(["bash", str(copy_dir / "install.sh")], env=self.env,
                                 capture_output=True, text=True, timeout=60)
            self.assertNotEqual(run.returncode, 0)
            self.assertFalse(self.dst.exists())
            self.assertFalse(self.state.exists())

    def test_install_refuses_hostile_rule_append_with_zero_state_change(self) -> None:
        # A hostile rule appended to the otherwise-approved file must be
        # refused BEFORE any destination/state mutation (exact-policy gate).
        with tempfile.TemporaryDirectory(prefix="usb-policy-hostile-") as tmp:
            copy_dir = Path(tmp) / "usb-policy"
            shutil.copytree(POLICY_DIR, copy_dir)
            rules = copy_dir / "70-android-auto-receiver.rules"
            rules.write_text(
                rules.read_text(encoding="utf-8")
                + '\nKERNEL=="*", MODE:="0666", ENV{ID_MM_DEVICE_IGNORE}="1"\n',
                encoding="utf-8")
            run = subprocess.run(["bash", str(copy_dir / "install.sh")], env=self.env,
                                 capture_output=True, text=True, timeout=60)
            self.assertNotEqual(run.returncode, 0)
            self.assertFalse(self.dst.exists())
            self.assertFalse(self.state.exists())

    def test_install_binds_to_source_snapshot(self) -> None:
        # Finding F3: a source swap after the hash must not change the
        # installed bytes nor the recorded digest - install consumes one
        # private snapshot captured before validation.
        with tempfile.TemporaryDirectory(prefix="usb-policy-swap-") as tmp:
            copy_dir = Path(tmp) / "usb-policy"
            shutil.copytree(POLICY_DIR, copy_dir)
            rules = copy_dir / "70-android-auto-receiver.rules"
            approved_bytes = rules.read_bytes()
            hostile = Path(tmp) / "hostile.rules"
            hostile.write_text(
                rules.read_text(encoding="utf-8") + '\nKERNEL=="*", MODE:="0666"\n',
                encoding="utf-8")
            env = dict(self.env,
                       AA_USB_POLICY_TEST_HOOKS="after-validate after-hash",
                       AA_USB_POLICY_TEST_HOOK_CMD=f'cp -- "{hostile}" "{rules}"')
            run = subprocess.run(["bash", str(copy_dir / "install.sh")], env=env,
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(self.dst.read_bytes(), approved_bytes,
                             "the validated snapshot must be installed, not the swapped source")
            expected_sha = hashlib.sha256(approved_bytes).hexdigest()
            manifest = (self.state / "manifest").read_text(encoding="utf-8")
            self.assertIn(f"installed_sha256={expected_sha}", manifest,
                          "the recorded digest must be the digest of the installed bytes")
            self.assertEqual(hashlib.sha256(self.dst.read_bytes()).hexdigest(), expected_sha)

            manifest_before = (self.state / "manifest").read_bytes()
            run2 = subprocess.run(["bash", str(copy_dir / "install.sh")], env=self.env,
                                  capture_output=True, text=True, timeout=60)
            self.assertNotEqual(run2.returncode, 0,
                                "a rerun against the swapped hostile source must be refused")
            self.assertEqual((self.state / "manifest").read_bytes(), manifest_before,
                             "a refused rerun must not rewrite the saved state")

    def test_install_refuses_dangling_manifest_link(self) -> None:
        # Finding F1: a dangling manifest symlink must never be read as "no
        # state" - the fresh-install write would follow it and CREATE the
        # link's target.
        self.state.mkdir(parents=True)
        victim = self.root / "victim.rules"
        (self.state / "manifest").symlink_to(victim)
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertFalse(victim.exists(), "the dangling link's target must never be created")
        self.assertFalse(self.dst.exists())
        self.assertTrue((self.state / "manifest").is_symlink())

    def test_rollback_refuses_dangling_manifest_link(self) -> None:
        self.state.mkdir(parents=True)
        victim = self.root / "victim.rules"
        (self.state / "manifest").symlink_to(victim)
        run = self.rollback()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self.assertFalse(victim.exists(), "the dangling link's target must never be created")

    def test_install_refuses_fifo_manifest_entry(self) -> None:
        self.state.mkdir(parents=True)
        os.mkfifo(self.state / "manifest")
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("not a regular file", run.stderr)

    def test_install_refuses_fifo_destination(self) -> None:
        self.dst.parent.mkdir(parents=True)
        os.mkfifo(self.dst)
        run = self.install()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("not a regular file", run.stderr)
        self.assertFalse(self.state.exists())

    def _swap_target(self) -> tuple[Path, bytes]:
        victim = self.root / "innocent-bystander.rules"
        victim.write_bytes(b"# innocent bystander\n")
        victim.chmod(0o600)
        return victim, b"# innocent bystander\n"

    def _assert_victim_untouched(self, victim: Path, content: bytes) -> None:
        self.assertEqual(victim.read_bytes(), content,
                         "the swap target's content must never change")
        self.assertEqual(victim.stat().st_mode & 0o777, 0o600,
                         "the swap target's mode must never change")

    def test_install_survives_symlink_swap_between_check_and_copy(self) -> None:
        # Finding F1 TOCTOU: swap the destination for a link to a victim file
        # after the trust/type checks and before the install copy.
        victim, content = self._swap_target()
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-check-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD=f'ln -sfn -- "{victim}" "{self.dst}"')
        run = self.run_script("install.sh", env)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self._assert_victim_untouched(victim, content)
        self.assertTrue(self.dst.is_symlink(), "the swapped link must not be written through")
        self.assertFalse((self.state / "manifest").exists())

    def test_install_survives_symlink_swap_between_copy_and_chmod(self) -> None:
        # The staged publish removed the old copy-then-chmod-the-destination
        # window (the chmod hits the private staged file only). This fixture
        # swaps at the analogous point - after the staged copy + chmod, before
        # the rename - and requires the same victim safety. The interrupted
        # transaction must leave the recoverable phase=installing record, not
        # a manifest claiming a completed install.
        victim, content = self._swap_target()
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-copy-stage",
                   AA_USB_POLICY_TEST_HOOK_CMD=f'ln -sfn -- "{victim}" "{self.dst}"')
        run = self.run_script("install.sh", env)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self._assert_victim_untouched(victim, content)
        self.assertTrue(self.dst.is_symlink(), "the swapped link must not be written through")
        manifest = self.state / "manifest"
        if manifest.exists():
            self.assertIn("phase=installing", manifest.read_text(encoding="utf-8"),
                          "an interrupted publish must leave the recoverable phase record")

    def test_rollback_survives_symlink_swap_before_restore(self) -> None:
        self.dst.parent.mkdir(parents=True)
        self.dst.write_bytes(b"# prior\n")
        self.assertEqual(self.install().returncode, 0)
        victim, content = self._swap_target()
        env = dict(self.env,
                   AA_USB_POLICY_TEST_HOOKS="after-check-dst",
                   AA_USB_POLICY_TEST_HOOK_CMD=f'ln -sfn -- "{victim}" "{self.dst}"')
        run = self.run_script("rollback.sh", env)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("symlink", run.stderr)
        self._assert_victim_untouched(victim, content)
        self.assertTrue((self.state / "prior.rules").exists(), "state must survive the refusal")
        self.assertTrue((self.state / "manifest").exists())

    def test_install_refuses_missing_rules_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="usb-policy-nofile-") as tmp:
            copy_dir = Path(tmp) / "usb-policy"
            shutil.copytree(POLICY_DIR, copy_dir)
            (copy_dir / "70-android-auto-receiver.rules").unlink()
            run = subprocess.run(["bash", str(copy_dir / "install.sh")], env=self.env,
                                 capture_output=True, text=True, timeout=60)
            self.assertNotEqual(run.returncode, 0)
            self.assertFalse(self.dst.exists())

    def test_scripts_pass_bash_n(self) -> None:
        for script in ("install.sh", "rollback.sh", "common.sh"):
            with self.subTest(script=script):
                run = subprocess.run(["bash", "-n", str(POLICY_DIR / script)],
                                     capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)

    def test_live_root_ancestry_accepts_root(self) -> None:
        # Given
        command = f'set -euo pipefail; source "{POLICY_DIR / "common.sh"}"; ROOT=""; die() {{ exit 91; }}; check_dir_trusted() {{ printf "%s\\n" "$1"; }}; trusted_chain_check /etc'
        # When
        run = subprocess.run(["bash", "-c", command], capture_output=True, text=True, timeout=30)
        # Then
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.splitlines(), ["/", "/etc"])

    def test_install_interrupt_at_each_recovery_window(self) -> None:
        checkpoints = ("after-stage-prior", "after-stage-state", "after-record-installing",
                       "after-stage-dst", "after-publish-dst", "after-record-installed",
                       "before-reload")
        for prior_exists in (False, True):
            for checkpoint in checkpoints:
                with self.subTest(prior=prior_exists, checkpoint=checkpoint):
                    # Given
                    with tempfile.TemporaryDirectory(prefix="usb-interrupt-") as tmp:
                        root = Path(tmp)
                        dst = root / "etc/udev/rules.d" / RULES_FILE.name
                        dst.parent.mkdir(parents=True)
                        baseline = b"# original baseline\n"
                        if prior_exists:
                            dst.write_bytes(baseline)
                        env = dict(self.env, AA_USB_POLICY_ROOT=tmp)
                        fault = dict(env, AA_USB_POLICY_TEST_HOOKS=checkpoint,
                                     AA_USB_POLICY_TEST_HOOK_CMD="exit 137")
                        # When
                        interrupted = self.run_script("install.sh", fault)
                        # Then
                        self.assertEqual(interrupted.returncode, 137,
                                         interrupted.stdout + interrupted.stderr)
                        backup = root / "var/lib/android-auto-receiver/usb-policy/prior.rules"
                        if backup.exists():
                            self.assertEqual(backup.read_bytes(), baseline)
                        recovered = self.run_script("rollback.sh", env)
                        self.assertEqual(recovered.returncode, 0,
                                         recovered.stdout + recovered.stderr)
                        self.assertEqual(dst.read_bytes() if dst.exists() else None,
                                         baseline if prior_exists else None)

    def test_rollback_interrupt_at_each_recovery_window(self) -> None:
        checkpoints = ("after-record-restoring", "after-stage-dst", "after-restore-dst",
                       "after-record-restored", "before-reload", "after-reload",
                       "after-retire-state")
        for prior_exists in (False, True):
            for checkpoint in checkpoints:
                if checkpoint == "after-stage-dst" and not prior_exists:
                    continue
                with self.subTest(prior=prior_exists, checkpoint=checkpoint):
                    # Given
                    with tempfile.TemporaryDirectory(prefix="usb-interrupt-") as tmp:
                        root = Path(tmp)
                        dst = root / "etc/udev/rules.d" / RULES_FILE.name
                        dst.parent.mkdir(parents=True)
                        baseline = b"# original baseline\n"
                        if prior_exists:
                            dst.write_bytes(baseline)
                        env = dict(self.env, AA_USB_POLICY_ROOT=tmp)
                        installed = self.run_script("install.sh", env)
                        self.assertEqual(installed.returncode, 0,
                                         installed.stdout + installed.stderr)
                        fault = dict(env, AA_USB_POLICY_TEST_HOOKS=checkpoint,
                                     AA_USB_POLICY_TEST_HOOK_CMD="exit 137")
                        # When
                        interrupted = self.run_script("rollback.sh", fault)
                        # Then
                        self.assertEqual(interrupted.returncode, 137,
                                         interrupted.stdout + interrupted.stderr)
                        backup = root / "var/lib/android-auto-receiver/usb-policy/prior.rules"
                        if prior_exists and checkpoint != "after-retire-state":
                            self.assertEqual(backup.read_bytes(), baseline)
                        recovered = self.run_script("rollback.sh", env)
                        self.assertEqual(recovered.returncode, 0,
                                         recovered.stdout + recovered.stderr)
                        self.assertEqual(dst.read_bytes() if dst.exists() else None,
                                         baseline if prior_exists else None)

    def test_sigkill_preserves_recovery_record(self) -> None:
        for script, checkpoint in (("install.sh", "after-publish-dst"),
                                   ("rollback.sh", "after-restore-dst"),
                                   ("rollback.sh", "after-retire-state")):
            with self.subTest(script=script, checkpoint=checkpoint):
                # Given
                with tempfile.TemporaryDirectory(prefix="usb-kill-") as tmp:
                    root = Path(tmp)
                    dst = root / "etc/udev/rules.d" / RULES_FILE.name
                    dst.parent.mkdir(parents=True)
                    baseline = b"# original baseline\n"
                    dst.write_bytes(baseline)
                    env = dict(self.env, AA_USB_POLICY_ROOT=tmp)
                    if script == "rollback.sh":
                        self.assertEqual(self.run_script("install.sh", env).returncode, 0)
                    fault = dict(env, AA_USB_POLICY_TEST_HOOKS=checkpoint,
                                 AA_USB_POLICY_TEST_HOOK_CMD='kill -KILL "$BASHPID"')
                    # When
                    interrupted = self.run_script(script, fault)
                    # Then
                    self.assertEqual(interrupted.returncode, -9)
                    backups = list((root / "var/lib/android-auto-receiver").glob("*/prior.rules"))
                    self.assertEqual([backup.read_bytes() for backup in backups], [baseline])
                    recovered = self.run_script("rollback.sh", env)
                    self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
                    self.assertEqual(dst.read_bytes(), baseline)

    def test_rollback_and_readme_do_not_overclaim_acl_revocation(self) -> None:
        # Finding F6: "stale ACLs cannot survive reload+trigger" is
        # unsupported (systemd uaccess refreshes an ACL only while a rule
        # tags the device at event time; removing our rule does not flush an
        # existing named-user ACL, and the distro's 51-android.rules
        # independently re-grants the same ids). The named limitation and
        # honest log wording are locked here.
        readme = (POLICY_DIR / "README.md").read_text(encoding="utf-8")
        self.assertIn("NOT revoked by rollback", readme)
        self.assertIn("independently authorized", readme)
        self.assertNotIn("no stale", readme)
        self.assertEqual(self.install().returncode, 0)
        run = self.rollback()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("named-user ACLs are NOT revoked by this tooling", run.stdout)
        self.assertNotIn("cannot survive", run.stdout + run.stderr)


class Udevadm(unittest.TestCase):
    """Recorded `udevadm test` fixtures: matched vs unmatched evidence."""

    def load(self, name: str) -> rules_check.UdevadmRun:
        return rules_check.parse_udevadm_test((FIXTURES / name).read_text(encoding="utf-8"))

    def test_matched_fixture_applies_policy(self) -> None:
        run = self.load("udevadm_matched.txt")
        self.assertTrue(run.assignments_for("70-android-auto-receiver.rules"),
                        "matched device must trigger our rules")
        self.assertEqual(run.properties.get("ID_MM_DEVICE_IGNORE"), "1")

    def test_unmatched_fixture_stays_at_default_policy(self) -> None:
        run = self.load("udevadm_unmatched.txt")
        self.assertEqual(run.assignments_for("70-android-auto-receiver.rules"), [])
        self.assertNotIn("ID_MM_DEVICE_IGNORE", run.properties)

    def test_pre_install_fixture_has_no_policy(self) -> None:
        run = self.load("udevadm_pre_install.txt")
        self.assertEqual(run.assignments_for("70-android-auto-receiver.rules"), [])
        self.assertNotIn("ID_MM_DEVICE_IGNORE", run.properties)

    def test_post_rollback_fixture_is_restored(self) -> None:
        run = self.load("udevadm_post_rollback.txt")
        self.assertEqual(run.assignments_for("70-android-auto-receiver.rules"), [])
        self.assertNotIn("ID_MM_DEVICE_IGNORE", run.properties)

    def test_hostile_udevadm_text_is_not_parsed_as_properties(self) -> None:
        junk = "ID_MM_DEVICE_IGNORE=1\nnot a property line\nrun: 'evil'\n" \
               "lowercase=ignored\n1-2.1: /etc/udev/rules.d/70-android-auto-receiver.rules:9 GROUP plugdev\n"
        run = rules_check.parse_udevadm_test(junk)
        self.assertEqual(run.properties.get("ID_MM_DEVICE_IGNORE"), "1")
        self.assertNotIn("lowercase", run.properties)
        self.assertEqual(run.assignments_for("70-android-auto-receiver.rules"), ["GROUP plugdev"])

    def test_rule_and_property_parsing_shapes(self) -> None:
        text = (
            "1-2.1: /etc/udev/rules.d/70-android-auto-receiver.rules:25 GROUP plugdev\n"
            "1-2.1: /usr/lib/udev/rules.d/50-udev-default.rules:72 MODE 0664\n"
            "DEVPATH=/devices/usb1/1-2/1-2.1\n"
            "ID_MM_DEVICE_IGNORE=1\n"
            "TAGS=:uaccess:\n"
        )
        run = rules_check.parse_udevadm_test(text)
        self.assertEqual(run.properties["ID_MM_DEVICE_IGNORE"], "1")
        self.assertEqual(run.properties["DEVPATH"], "/devices/usb1/1-2/1-2.1")
        self.assertEqual(len(run.rule_lines), 2)
        self.assertEqual(run.assignments_for("70-android-auto-receiver.rules"), ["GROUP plugdev"])


if __name__ == "__main__":
    groups = {"rules": Rules, "rollback": Rollback, "udevadm": Udevadm}
    selected = [groups[sys.argv[1]]] if len(sys.argv) > 1 else groups.values()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(g) for g in selected)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
