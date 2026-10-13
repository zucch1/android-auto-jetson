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

    def test_install_twice_is_idempotent(self) -> None:
        self.assertEqual(self.install().returncode, 0)
        manifest_first = (self.state / "manifest").read_bytes()
        run = self.install()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("already-installed", run.stdout)
        self.assertEqual((self.state / "manifest").read_bytes(), manifest_first)
        self.assertEqual(self.dst.read_bytes(), RULES_FILE.read_bytes())

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
        for script in ("install.sh", "rollback.sh"):
            with self.subTest(script=script):
                run = subprocess.run(["bash", "-n", str(POLICY_DIR / script)],
                                     capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)


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
