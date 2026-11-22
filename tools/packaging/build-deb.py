#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble the Debian package from the receiver-deb CMake staging component."""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.build.hardening import check, readelf
from tools.tls.scan import rejection

NAME = "android-auto-receiver"
DOC = f"usr/share/doc/{NAME}"
LIB = f"usr/lib/{NAME}"
ARCHITECTURES = {"arm64": "AArch64", "amd64": "Advanced Micro Devices X86-64"}
APPS = (
    "aa-wireless-receiver", "aa-wired-receiver", "aa-wired-enrol", "aa-dummy-display",
    "aa-decode-probe", "aa-usb-aoa-probe", "aa-ipc-bench",
)
# Runtime instructions and templates are selected individually. Operational
# captures, the sysroot, and operator-provisioned configuration are outside it.
DATA = {
    "packaging/systemd/aa-receiver.service": "usr/lib/systemd/user/aa-receiver.service",
    "packaging/systemd/aa-system-helper.service": "usr/lib/systemd/system/aa-system-helper.service",
    "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf":
        "usr/share/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf",
    "tools/usb-policy/70-android-auto-receiver.rules":
        "usr/lib/udev/rules.d/70-android-auto-receiver.rules",
    "packaging/ap.conf.example": f"{DOC}/examples/ap.conf.example",
    "packaging/debian/copyright": f"{DOC}/copyright",
    "THIRD_PARTY_NOTICES.md": f"{DOC}/THIRD_PARTY_NOTICES.md",
    "tools/aa-decode-probe/README.md": f"{DOC}/decode-probe.md",
    **{f"docs/{doc}": f"{DOC}/{doc}" for doc in (
        "debian-package.md", "HOST_BUILD.md", "wired-enrolment.md", "wireless-handoff.md", "system-helper.md",
        "tls-credentials.md", "networkmanager-ap.md", "task36-bluez.md",
        "dbus/README.md", "dbus/org.custom.AndroidAutoReceiver1.xml",
        "dbus/org.custom.AndroidAutoSystemHelper1.xml",
    )},
}


def git(*args):
    return subprocess.check_output(["git", "-C", ROOT, *args], text=True).strip()


def package_version():
    # Only plain vMAJOR.MINOR.PATCH tags are releases; pre-release tags such as
    # v1.0.0-rc1 are skipped so describe falls through to an earlier release or
    # to the 0.0.0+git fallback instead of aborting.
    tag = git("describe", "--long", "--always", "--match", "v[0-9]*.[0-9]*.[0-9]*",
              "--exclude", "*-*", "--abbrev=12")
    match = re.fullmatch(r"v(\d+\.\d+\.\d+)-(\d+)-g([0-9a-f]+)", tag)
    if match:
        release, distance, commit = match.groups()
        return release if distance == "0" else f"{release}+git{distance}.g{commit}"
    if not re.fullmatch(r"[0-9a-f]+", tag):
        raise ValueError("expected a vMAJOR.MINOR.PATCH release tag")
    return f"0.0.0+git{git('rev-list', '--count', 'HEAD')}.g{tag}"


def regular_file(path, root):
    if path.resolve() != path or not path.is_file() or not path.is_relative_to(root):
        raise ValueError(f"missing or non-regular package input: {path}")
    return path


def copy_file(source, destination, mode=0o644):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)


def write_metadata(path, data):
    path.write_bytes(data)
    path.chmod(0o644)


def validate_elf(path, architecture, runtime_path):
    header = readelf(path, "-h")
    machines = re.findall(r"^\s*Machine:\s*(.+)$", header, re.MULTILINE)
    classes = re.findall(r"^\s*Class:\s*(.+)$", header, re.MULTILINE)
    if not machines or set(machines) != {ARCHITECTURES[architecture]} or set(classes) != {"ELF64"}:
        raise ValueError(f"{path.name}: expected {architecture} ELF objects")
    if runtime_path is None:
        return ""
    dynamic = readelf(path, "-d")
    paths = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic)
    if paths != runtime_path:
        raise ValueError(f"{path.name}: unexpected runtime library search path")
    if re.search(r"\(NEEDED\).*?\[(?:/|lib(?:a|ub|t)san)", dynamic):
        raise ValueError(f"{path.name}: absolute or sanitizer runtime dependency")
    return dynamic


def stage_binaries(stage, payload, architecture):
    executables = [f"usr/bin/{app}" for app in APPS] + ["usr/libexec/aa-system-helper"]
    private_runpath = "$ORIGIN/../lib/android-auto-receiver"
    dynamic_sections = []
    private_sonames = set()
    for relative in executables:
        source = regular_file(stage / relative, stage)
        runpath = [private_runpath] if source.name in {"aa-wireless-receiver", "aa-wired-receiver"} else []
        dynamic_sections.append(validate_elf(source, architecture, runpath))
        failures = check(source)
        if failures:
            raise ValueError(f"{source.name}: {', '.join(failures)}")
        copy_file(source, payload / relative, 0o755)

    for library in ("libaasdk", "libaap_protobuf"):
        pattern = re.compile(rf"{library}\.so\.\d[0-9A-Za-z.+~-]*")
        candidates = [path for path in (stage / LIB).glob(f"{library}.so.*")
                      if pattern.fullmatch(path.name) and not path.is_symlink()]
        if len(candidates) != 1:
            raise ValueError(f"expected exactly one staged version of {library}")
        source = regular_file(candidates[0], stage)
        dynamic = validate_elf(source, architecture, ["$ORIGIN"])
        dynamic_sections.append(dynamic)
        soname = re.search(r"\(SONAME\).*?\[(.*?)\]", dynamic)
        if not soname or not pattern.fullmatch(soname[1]):
            raise ValueError(f"{library}: missing or invalid SONAME")
        private_sonames.add(soname[1])
        copy_file(source, payload / LIB / soname[1])

    for dynamic in dynamic_sections:
        required = set(re.findall(r"\(NEEDED\).*?\[(lib(?:aasdk|aap_protobuf)\.so[^\]]*)\]", dynamic))
        if missing := required - private_sonames:
            raise ValueError(f"missing private runtime libraries: {', '.join(sorted(missing))}")

    archive = f"{LIB}/libaareceiver-consumer.a"
    source = regular_file(stage / archive, stage)
    with source.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise ValueError(f"{source.name}: expected a regular static archive")
    validate_elf(source, architecture, None)
    copy_file(source, payload / archive)


def build(stage, output, architecture):
    version = package_version()
    subprocess.run(["dpkg", "--validate-version", version], check=True)
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", git("show", "-s", "--format=%ct", "HEAD")))
    date = datetime.fromtimestamp(epoch, timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")
    filename = f"{NAME}_{version}_{architecture}.deb"
    output.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=f".{NAME}-", dir=output) as scratch:
        scratch = Path(scratch)
        payload = scratch / NAME
        control = payload / "DEBIAN"
        control.mkdir(parents=True)
        stage_binaries(stage, payload, architecture)
        for source, destination in DATA.items():
            copy_file(regular_file(ROOT / source, ROOT), payload / destination)

        for script in ("postinst", "prerm", "postrm"):
            copy_file(regular_file(ROOT / "packaging/debian" / script, ROOT), control / script, 0o755)
        changelog = (
            f"{NAME} ({version}) noble; urgency=medium\n\n"
            "  * Build the Android Auto Receiver and reference tools.\n\n"
            f" -- Newton Ambele <59395246+zucch1@users.noreply.github.com>  {date}\n"
        )
        write_metadata(payload / DOC / "changelog.gz",
                       gzip.compress(changelog.encode(), mtime=0))

        files = sorted(path for path in (payload / "usr").rglob("*") if path.is_file())
        installed_size = sum((path.stat().st_size + 1023) // 1024 for path in files)
        control_fields = regular_file(ROOT / "packaging/debian/control", ROOT).read_text()
        write_metadata(control / "control", (
            f"Version: {version}\nArchitecture: {architecture}\nInstalled-Size: {installed_size}\n"
            f"{control_fields}"
        ).encode())
        private_paths = (b"/home/", b"/root/", os.fsencode(ROOT), os.fsencode(stage))
        checksums = []
        for path in files:
            data = path.read_bytes()
            if any(private_path in data for private_path in private_paths):
                raise ValueError(f"private path in package input: {path.relative_to(payload)}")
            digest = hashlib.md5(data, usedforsecurity=False).hexdigest()
            checksums.append(f"{digest}  {path.relative_to(payload)}\n")
        write_metadata(control / "md5sums", "".join(checksums).encode())

        if reason := rejection(payload):
            raise ValueError(f"package credential scan failed: {reason}")
        for path in [*payload.rglob("*"), payload]:
            if path.is_dir():
                path.chmod(0o755)
            os.utime(path, (epoch, epoch))

        subprocess.run(["dpkg-deb", "--root-owner-group", "--uniform-compression", "-Zxz",
                        "-z6", "--threads-max=1", "--build", payload, scratch / filename],
                       check=True, env={**os.environ, "SOURCE_DATE_EPOCH": str(epoch)})
        (scratch / filename).replace(output / filename)
    print(output / filename)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path, help="DESTDIR populated by the receiver-deb component")
    parser.add_argument("output", type=Path, help="directory for the resulting .deb")
    parser.add_argument("--arch", choices=ARCHITECTURES, default="arm64",
                        help="package architecture (amd64 is for layout verification)")
    args = parser.parse_args()
    os.environ["LC_ALL"] = "C"
    try:
        build(args.stage.resolve(strict=True), args.output.resolve(), args.arch)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"build-deb: {error}\n")


if __name__ == "__main__":
    main()
