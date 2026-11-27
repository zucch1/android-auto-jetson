# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the package assembler with hardened ELF fixtures or the staged install."""

import argparse
from configparser import ConfigParser
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAME = "android-auto-receiver"
DOC = f"usr/share/doc/{NAME}"
LIB = f"usr/lib/{NAME}"
POLICY = "usr/share/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf"
USER_UNIT = "usr/lib/systemd/user/aa-receiver.service"
UDEV_RULE = "usr/lib/udev/rules.d/70-android-auto-receiver.rules"
FIXTURE_SOURCE = "int main(void) { volatile char buf[32]; buf[0] = 0; return buf[0]; }\n"
EXECUTABLES = {
    "usr/bin/aa-wireless-receiver", "usr/bin/aa-wired-receiver", "usr/bin/aa-wired-enrol",
    "usr/bin/aa-dummy-display", "usr/bin/aa-decode-probe", "usr/bin/aa-usb-aoa-probe",
    "usr/bin/aa-ipc-bench", "usr/libexec/aa-system-helper",
}
LIBRARIES = {f"{LIB}/libaasdk.so.0", f"{LIB}/libaap_protobuf.so.0", f"{LIB}/libaareceiver-consumer.a"}
FILES = EXECUTABLES | LIBRARIES | {
    USER_UNIT, "usr/lib/systemd/system/aa-system-helper.service", POLICY,
    UDEV_RULE,
} | {f"{DOC}/{doc}" for doc in (
    "changelog.gz", "copyright", "THIRD_PARTY_NOTICES.md", "debian-package.md", "HOST_BUILD.md",
    "decode-probe.md", "wired-enrolment.md", "wireless-handoff.md", "system-helper.md",
    "tls-credentials.md", "networkmanager-ap.md", "task36-bluez.md",
    "examples/ap.conf.example", "dbus/README.md", "dbus/org.custom.AndroidAutoReceiver1.xml",
    "dbus/org.custom.AndroidAutoSystemHelper1.xml",
)}


def run(*command, **kwargs):
    return subprocess.check_output(command, **kwargs)


def fixture_executable(cc, target, *link_flags):
    run(cc, "-fstack-protector-all", "-Wl,-z,relro,-z,now", "-Wl,-z,noexecstack",
        "-s", "-fPIE", "-pie", "-x", "c", "-o", target, "-", *link_flags,
        input=FIXTURE_SOURCE, text=True)


def fixture(stage, cc, ar):
    for relative in EXECUTABLES | LIBRARIES:
        (stage / relative).parent.mkdir(parents=True, exist_ok=True)
    for soname in ("libaasdk.so.0", "libaap_protobuf.so.0"):
        run(cc, "-shared", "-s", "-fPIC", f"-Wl,-soname,{soname}", "-Wl,-rpath,$ORIGIN",
            "-x", "c", "-o", stage / LIB / (soname + ".0"), "-", input=FIXTURE_SOURCE, text=True)
        (stage / LIB / soname).symlink_to(soname + ".0")
    for relative in sorted(EXECUTABLES):
        private_link = []
        if relative.endswith("-receiver"):
            private_link = ["-Wl,-rpath,$ORIGIN/../lib/android-auto-receiver",
                            "-Wl,--no-as-needed", "-L", stage / LIB,
                            "-l:libaasdk.so.0", "-l:libaap_protobuf.so.0"]
        fixture_executable(cc, stage / relative, *private_link)
    obj = stage / "fixture.o"
    run(cc, "-c", "-x", "c", "-o", obj, "-", input=FIXTURE_SOURCE, text=True)
    run(ar, "rcs", stage / LIB / "libaareceiver-consumer.a", obj)
    for relative in ("home/private.pem", "sysroot/usr/lib/libhost.so", "etc/android-auto-receiver/ap.conf",
                     "var/lib/android-auto-receiver/approved-phones.json",
                     "usr/lib/systemd/user/default.target.wants/aa-receiver.service"):
        decoy = stage / relative
        decoy.parent.mkdir(parents=True, exist_ok=True)
        decoy.write_text("fixture private data")


def inspect(package, owner, arch):
    info = run("dpkg-deb", "--info", package, text=True)
    for field in (f"Package: {NAME}", f"Architecture: {arch}", "Version: ",
                  "Depends: ", "Maintainer: Newton Ambele", "postinst"):
        assert field in info, info
    version = run("dpkg-deb", "--field", package, "Version", text=True).strip()
    assert package.name == f"{NAME}_{version}_{arch}.deb"
    depends = run("dpkg-deb", "--field", package, "Depends", text=True)
    for dependency in ("libssl3t64", "libqt6widgets6t64", "libusb-1.0-0", "bluez", "network-manager"):
        assert dependency in depends, depends

    listing = run("dpkg-deb", "--contents", package, text=True)
    assert "./" + USER_UNIT in listing and "./" + POLICY in listing
    output = run("dpkg-deb", "--fsys-tarfile", package)
    with tarfile.open(fileobj=io.BytesIO(output)) as archive:
        actual = {m.name.removeprefix("./"): m for m in archive if not m.isdir()}
        assert set(actual) == FILES, sorted(set(actual) ^ FILES)
        for path, member in actual.items():
            assert member.isfile() and (member.uid, member.gid) == (0, 0), member
            assert member.mode == (0o755 if path in EXECUTABLES else 0o644), member
            data = archive.extractfile(member).read()
            assert all(value not in data for value in (b"fixture private data", b"/home/", b"/root/")), path
        assert archive.extractfile(actual[POLICY]).read() == (ROOT / "packaging/dbus" / Path(POLICY).name).read_bytes()
        receiver_unit = ConfigParser(interpolation=None)
        receiver_unit.read_string(archive.extractfile(actual[USER_UNIT]).read().decode())
        assert receiver_unit["Install"]["WantedBy"] == "default.target"
        assert "PartOf" not in receiver_unit["Unit"]
        assert receiver_unit["Unit"]["Wants"] == "dbus.socket"
        assert receiver_unit["Unit"]["After"] == "dbus.socket"
        assert receiver_unit["Service"]["Restart"] == "on-failure"
        assert receiver_unit["Service"].getint("RestartSec") == 2
        assert receiver_unit["Service"].getint("RestartPreventExitStatus") == 2
        assert receiver_unit["Unit"].getint("StartLimitIntervalSec") == 60
        assert receiver_unit["Unit"].getint("StartLimitBurst") == 5

    extracted = owner / "root"
    control = owner / "control"
    run("dpkg-deb", "--extract", package, extracted)
    run("dpkg-deb", "--control", package, control)
    assert {p.name for p in control.iterdir()} == {"control", "md5sums", "postinst", "prerm", "postrm"}
    assert (control / "control").read_text().splitlines()[0] == f"Package: {NAME}"
    run("md5sum", "--check", control / "md5sums", cwd=extracted)
    inspect_maintainer_scripts(control, extracted, owner)


def inspect_maintainer_scripts(control, extracted, owner):
    script_env = {**os.environ, "DPKG_ROOT": str(extracted), "PATH": "/nonexistent"}
    run(control / "postinst", "configure", env=script_env)
    run(control / "prerm", "remove", env=script_env)
    run(control / "postrm", "purge", env=script_env)

    override = extracted / "etc/udev/rules.d" / Path(UDEV_RULE).name
    override.parent.mkdir(parents=True)
    for mask in (False, True):
        if mask:
            override.symlink_to("missing-mask-target")
        else:
            override.write_text("# operator override\n")
        shadowed = subprocess.run([control / "postinst", "configure"], env=script_env,
                                  text=True, capture_output=True, check=True)
        assert "shadows the packaged udev rule" in shadowed.stderr
        if mask:
            assert override.readlink() == Path("missing-mask-target")
        else:
            assert override.read_text() == "# operator override\n"
        override.unlink()

    inspect_udev_trigger(control / "postinst", extracted, owner)
    (extracted / POLICY).unlink()
    failed = subprocess.run([control / "postinst", "configure"], env=script_env,
                            text=True, stderr=subprocess.PIPE)
    assert failed.returncode and "missing packaged system-helper D-Bus policy" in failed.stderr


def inspect_udev_trigger(postinst, extracted, owner):
    # Redirect the live-path branch into the private package root and provide
    # only a logging udevadm shim on PATH.
    script = postinst.read_text()
    for path in ("/run/udev", "/sys/bus/usb/devices", f"/{POLICY}", "/etc/udev/rules.d"):
        script = script.replace(path, "${TEST_ROOT}" + path)
    (extracted / "run/udev").mkdir(parents=True)
    usb = extracted / "sys/bus/usb/devices"
    usb.mkdir(parents=True)
    approved = set(re.findall(r'ATTR\{idVendor\}=="(\w+)", ATTR\{idProduct\}=="(\w+)"',
                              (extracted / UDEV_RULE).read_text()))
    assert approved
    for vendor, product in sorted(approved | {("04e8", "0000"), ("1d6b", "0002")}):
        device = usb / f"{vendor}-{product}"
        device.mkdir()
        (device / "idVendor").write_text(f"{vendor}\n")
        (device / "idProduct").write_text(f"{product}\n")
    (usb / "interface-without-ids").mkdir()

    shims = owner / "shims"
    shims.mkdir()
    udevadm = shims / "udevadm"
    udevadm.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$UDEV_LOG"\n')
    udevadm.chmod(0o755)
    log = owner / "udev.log"
    result = subprocess.run(["/bin/sh", "-s", "configure"], input=script, check=True, text=True,
                            capture_output=True, env={"PATH": str(shims), "UDEV_LOG": str(log),
                                                      "TEST_ROOT": str(extracted)})
    assert not result.stderr, result.stderr
    assert log.read_text().splitlines() == ["control --reload", *(
        f"trigger --action=change --parent-match={usb / f'{vendor}-{product}'} "
        "--subsystem-match=usb --subsystem-match=tty"
        for vendor, product in sorted(approved)
    )]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cc")
    parser.add_argument("ar")
    parser.add_argument("--arch", choices=("amd64", "arm64"))
    parser.add_argument("--lintian", action="store_true")
    parser.add_argument("--build-dir", type=Path, help="install the real receiver-deb component")
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    required_tools = ["dpkg", "dpkg-deb", "readelf"]
    if args.lintian:
        required_tools.append("lintian")
    missing = [tool for tool in required_tools if not shutil.which(tool)]
    if missing:
        print(f"SKIP: unavailable tools: {', '.join(missing)}")
        return 77

    arch = args.arch or run("dpkg", "--print-architecture", text=True).strip()
    with tempfile.TemporaryDirectory(prefix="aa-deb-test-") as directory:
        owner = Path(directory)
        stage = owner / "stage"
        if args.build_dir:
            run(args.cmake, "--install", args.build_dir, "--prefix", "/usr",
                "--component", "receiver-deb", "--strip", env={**os.environ, "DESTDIR": str(stage)})
        else:
            fixture(stage, args.cc, args.ar)
        build = [ROOT / "tools/packaging/build-deb.py", "--arch", arch, stage]
        run(*build, owner / "first")
        package, = (owner / "first").glob("*.deb")
        if args.lintian:
            subprocess.run(["lintian", "--fail-on", "error", package], check=True)
            return 0
        inspect(package, owner, arch)
        if args.build_dir:
            return 0
        run(*build, owner / "second", env={**os.environ, "TZ": "Pacific/Auckland"}, umask=0o077)
        assert package.read_bytes() == (owner / "second" / package.name).read_bytes()
        if arch == "amd64":
            wrong = subprocess.run([*build, owner / "wrong", "--arch", "arm64"],
                                   text=True, stderr=subprocess.PIPE)
            assert wrong.returncode and "expected arm64 ELF objects" in wrong.stderr
            assert not list((owner / "wrong").glob("*.deb"))

        target = stage / "usr/bin/aa-decode-probe"
        clean = target.read_bytes()
        for injection, diagnostic in (
            (b"\n/home/package-fixture/checkout\n", "private path in package input"),
            (b"\n/tmp/package-fixture/checkout\n", "private path in package input"),
            (b"\n/opt/package-fixture/checkout\n", "private path in package input"),
            (b"\n/src/package-fixture/checkout\n", "private path in package input"),
            (b"\n-----" + b"BEGIN PRIVATE KEY-----\n", "package credential scan failed"),
        ):
            target.write_bytes(clean + injection)
            rejected = subprocess.run([*build, owner / "rejected"], text=True, stderr=subprocess.PIPE)
            assert rejected.returncode and diagnostic in rejected.stderr, rejected.stderr
            assert not list((owner / "rejected").glob("*.deb"))

        # Merely shipping a matching SONAME is insufficient when this
        # executable has no search path to the private library directory.
        fixture_executable(args.cc, target, "-Wl,--no-as-needed", "-L", stage / LIB, "-l:libaasdk.so.0")
        rejected = subprocess.run([*build, owner / "unresolved"], text=True, stderr=subprocess.PIPE)
        assert rejected.returncode and "libaasdk.so.0 does not resolve" in rejected.stderr, rejected.stderr
        assert not list((owner / "unresolved").glob("*.deb"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
