# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/policy_bus.py build/host-dev
"""Exercise the packaged delivery policy on isolated, unprivileged buses."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from private_bus import line, process


def verify_delivery(build: Path, directory: Path, provisioned: bool) -> None:
    name = "org.custom.AndroidAutoSystemHelper"
    interface = "org.custom.AndroidAutoSystemHelper1"
    config = ET.Element("busconfig")
    ET.SubElement(config, "type").text = "session"
    ET.SubElement(config, "listen").text = f"unix:abstract=aa34-policy-{os.getpid()}-{int(provisioned)}"
    ET.SubElement(config, "auth").text = "EXTERNAL"
    baseline = ET.SubElement(config, "policy", context="default")
    ET.SubElement(baseline, "allow", own="*")
    ET.SubElement(baseline, "allow", send_destination="*")
    ET.SubElement(baseline, "allow", receive_sender="*")
    ET.SubElement(config, "include").text = str(
        Path(__file__).resolve().parents[2] / "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf"
    )
    account = ET.SubElement(config, "policy", user=str(os.getuid()))
    # The recording fixture is unprivileged; only this test grants it ownership.
    ET.SubElement(account, "allow", own=name)
    if provisioned:
        ET.SubElement(account, "allow", send_destination=name,
                      send_interface=interface, send_type="method_call")
    config_path = directory / f"bus-{int(provisioned)}.conf"
    ET.ElementTree(config).write(config_path, encoding="utf-8", xml_declaration=True)
    env = dict(os.environ)
    with process(["dbus-daemon", "--nofork", "--print-address=1", f"--config-file={config_path}"], env) as bus:
        address = line(bus)
        env["DBUS_SESSION_BUS_ADDRESS"] = address
        with process([str(build / "aa_system_helper_fixture"), "helper", address,
                      str(directory), str(directory / "effects")], env) as helper:
            assert line(helper) == "ready"
            result = subprocess.run(
                ["dbus-send", "--session", "--print-reply", "--reply-timeout=2000",
                 f"--dest={name}", "/org/custom/AndroidAutoSystemHelper",
                 f"{interface}.Execute", "uint32:4", "uint32:0", "uint64:0"],
                env=env, capture_output=True, text=True, timeout=5, check=False,
            )
            if provisioned:
                # Delivery succeeds, but this executable is not the receiver.
                assert result.returncode == 0, result.stderr
                assert "uint32 1001" in result.stdout, result.stdout
                print("provisioned_uid_delivery=allowed; wrong_executable=helper_rejected", flush=True)
            else:
                assert result.returncode != 0, result.stdout
                assert "org.freedesktop.DBus.Error.AccessDenied" in result.stderr, result.stderr
                print("default_delivery=AccessDenied", flush=True)


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="helper-policy-", dir=build) as temporary:
        directory = Path(temporary)
        fixture = build / "aa_system_helper_fixture"
        (directory / "session.uid").write_text(f"{os.getuid()}\n")
        (directory / "receiver.sha256").write_text(hashlib.sha256(fixture.read_bytes()).hexdigest() + "\n")
        for filename in ("session.uid", "receiver.sha256"):
            (directory / filename).chmod(0o600)
        verify_delivery(build, directory, provisioned=False)
        verify_delivery(build, directory, provisioned=True)


if __name__ == "__main__":
    main()
