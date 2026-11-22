#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/contract/test_integration_contract.py
"""Positive and negative drift tests for tools/contract/check_integration.py."""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools/contract/check_integration.py"

COPIED_FILES = (
    "docs/INTEGRATION-CONTRACT.md",
    "docs/dbus/org.custom.AndroidAutoReceiver1.xml",
    "docs/dbus/org.custom.AndroidAutoSystemHelper1.xml",
    "docs/system-helper.md",
    "docs/wired-enrolment.md",
    "docs/video-pipeline.md",
    "docs/tls-credentials.md",
    "docs/wireless-handoff.md",
    "include/aa/ipc/Contract.hpp",
    "include/aa/ipc/Control.hpp",
    "include/aa/ipc/PeerCredentials.hpp",
    "include/aa/ipc/VideoSocket.hpp",
    "include/aa/ipc/VideoChannel.hpp",
    "include/aa/ipc/InputSocket.hpp",
    "include/aa/ipc/InputChannel.hpp",
    "include/aa/helper/BusService.hpp",
    "include/aa/helper/SystemHelper.hpp",
    "include/aa/consumer/Decode.hpp",
    "include/aa/config/Screen.hpp",
    "include/aa/config/VideoProfiles.hpp",
    "include/aa/protocol/Protocol.hpp",
    "include/aa/protocol/Messages.hpp",
    "include/aa/audio/Audio.hpp",
    "include/aa/audio/AudioMix.hpp",
    "include/aa/audio/AudioRouter.hpp",
    "src/ipc/ControlService.cpp",
    "src/ipc/ControlPairing.cpp",
    "src/ipc/ControlComparison.cpp",
    "src/ipc/PairingGate.cpp",
    "src/ipc/dbus_adapter/BusWire.hpp",
    "src/ipc/dbus_adapter/ControlDispatcher.cpp",
    "src/ipc/dbus_adapter/DispatchReply.hpp",
    "src/ipc/video/VideoCodec.cpp",
    "src/ipc/video/VideoChannel.cpp",
    "src/ipc/input/InputCodec.cpp",
    "src/ipc/input/InputChannel.cpp",
    "src/input/Capability.cpp",
    "src/input/InputService.cpp",
    "src/display/DisplayController.cpp",
    "src/display/qt_adapter/ProjectionWidget.cpp",
    "src/helper/SystemHelper.cpp",
    "src/app/WiredSession.cpp",
    "packaging/systemd/aa-system-helper.service",
    "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf",
    "packaging/ap.conf.example",
    "tools/usb-policy/70-android-auto-receiver.rules",
    "tools/ipc/dbus_contract.py",
    "tools/ipc/dbus_contract_schema.py",
    "tools/qualification/check_privacy.py",
    "tools/contract/check_integration.py",
    "tools/dummy-display/main.cpp",
    "tools/wired-enrol/main.cpp",
    "tools/wired-receiver/main.cpp",
    "tools/wireless-receiver/Runtime.hpp",
    "tools/wireless-receiver/ServiceDescription.hpp",
    "tools/wireless-receiver/AdmissionEvents.hpp",
    "tools/wireless-receiver/Options.hpp",
)


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(CHECKER), "--root", str(root)],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


def stage_tree(dst: Path) -> None:
    for rel in COPIED_FILES:
        src = REPO_ROOT / rel
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)


class IntegrationContractTests(unittest.TestCase):
    def test_real_repo_passes(self) -> None:
        res = run_checker(REPO_ROOT)
        self.assertEqual(res.returncode, 0, msg=res.stdout + res.stderr)
        self.assertIn("integration-contract PASS", res.stdout)

    def test_staged_baseline_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            res = run_checker(root)
            self.assertEqual(res.returncode, 0, msg=res.stdout + res.stderr)

    def test_receiver_xml_method_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            xml_path = root / "docs/dbus/org.custom.AndroidAutoReceiver1.xml"
            xml_path.write_text(
                xml_path.read_text(encoding="utf-8").replace(
                    '<method name="Ping">', '<method name="PingV2">'
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_RECEIVER_METHOD_DRIFT", res.stdout)

    def test_runtime_receiver_object_path_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            hdr = root / "include/aa/ipc/Contract.hpp"
            hdr.write_text(
                hdr.read_text(encoding="utf-8").replace(
                    'kContractObjectPath = "/org/custom/AndroidAutoReceiver";',
                    'kContractObjectPath = "/org/custom/AndroidAutoReceiverDrift";',
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_RECEIVER_PATH", res.stdout)

    def test_receiver_error_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            hdr = root / "src/ipc/dbus_adapter/DispatchReply.hpp"
            hdr.write_text(
                hdr.read_text(encoding="utf-8")
                + '\ninline constexpr std::string_view kNewErr = "org.custom.AndroidAutoReceiver1.Error.NewDrift";\n',
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_MISSING_ERROR", res.stdout)

    def test_helper_xml_semver_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            xml_path = root / "docs/dbus/org.custom.AndroidAutoSystemHelper1.xml"
            xml_path.write_text(
                xml_path.read_text(encoding="utf-8").replace('value="1.5.0"', 'value="1.6.0"'),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_HELPER_SEMVER", res.stdout)

    def test_video_framing_constant_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            hdr = root / "include/aa/ipc/VideoSocket.hpp"
            hdr.write_text(
                hdr.read_text(encoding="utf-8").replace(
                    "kFrameHeaderBytes = 56;", "kFrameHeaderBytes = 64;"
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_VIDEO_CONSTANT_DRIFT", res.stdout)

    def test_video_frame_timestamp_offset_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            codec = root / "src/ipc/video/VideoCodec.cpp"
            codec.write_text(
                codec.read_text(encoding="utf-8").replace(
                    "put(record_prefix, 36, header.timestamp_ns, 8);",
                    "put(record_prefix, 35, header.timestamp_ns, 8);",
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT", res.stdout)

    def test_input_framing_constant_drift_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            hdr = root / "include/aa/ipc/InputSocket.hpp"
            hdr.write_text(
                hdr.read_text(encoding="utf-8").replace(
                    "kInputRecordBytes = 24;", "kInputRecordBytes = 32;"
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_INPUT_CONSTANT_DRIFT", res.stdout)

    def test_helper_service_user_nobody_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            unit = root / "packaging/systemd/aa-system-helper.service"
            unit.write_text(
                unit.read_text(encoding="utf-8").replace("[Service]\n", "[Service]\nUser=nobody\n", 1),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_UNIT_DRIFT", res.stdout)

    def test_helper_dbus_policy_default_allow_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            conf = root / "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf"
            conf.write_text(
                conf.read_text(encoding="utf-8").replace(
                    '<deny send_destination="org.custom.AndroidAutoSystemHelper"/>',
                    '<allow send_destination="org.custom.AndroidAutoSystemHelper"/>',
                ),
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_DBUS_POLICY_DRIFT", res.stdout)

    def test_missing_systemd_unit_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            (root / "packaging/systemd/aa-system-helper.service").unlink()
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_MISSING_UNIT_FILE", res.stdout)

    def test_broken_doc_reference_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage_tree(root)
            doc = root / "docs/INTEGRATION-CONTRACT.md"
            doc.write_text(
                doc.read_text(encoding="utf-8") + "\nSee `include/aa/ipc/DoesNotExist.hpp`.\n",
                encoding="utf-8",
            )
            res = run_checker(root)
            self.assertEqual(res.returncode, 1, msg=res.stdout + res.stderr)
            self.assertIn("INTEGRATION_CONTRACT_BROKEN_REF", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
