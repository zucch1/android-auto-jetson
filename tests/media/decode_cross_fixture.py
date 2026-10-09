# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Imported by python3 -B tests/media/cross_decode_overlay.py --scratch BUILD
"""Public compile/link fixtures with synthetic GStreamer stubs; NEVER execute them."""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.sysroot.manifest import encode_manifest
from tools.sysroot.models import (Architecture, Digest, Identity, InputPath, Manifest, Package,
                                 PackageName, Provenance, RegularFile, RelativePath, Version)


def make_sysroot(base: Path) -> tuple[Path, Path]:
    """Stage host-installed cross runtime as an explicitly fixture-only hash-gated payload."""
    root = base / 'rootfs'
    shutil.copytree('/usr/aarch64-linux-gnu/include', root / 'usr/include')
    library = root / 'usr/lib/aarch64-linux-gnu'
    shutil.copytree('/usr/aarch64-linux-gnu/lib', library)
    stdcpp = subprocess.check_output(['aarch64-linux-gnu-g++', '-print-file-name=libstdc++.so'], text=True).strip()
    shutil.copyfile(stdcpp, library / 'libstdc++.so')
    for name in ('libc.so', 'libm.so'):
        path = library / name
        path.write_text(path.read_text().replace('/usr/aarch64-linux-gnu/lib/', '/usr/lib/aarch64-linux-gnu/'))
    package = PackageName('synthetic-cross-runtime')
    entries = tuple(RegularFile(InputPath('/' + path.relative_to(root).as_posix()),
                               RelativePath(path.relative_to(root).as_posix()), package,
                               Digest(hashlib.sha256(path.read_bytes()).hexdigest()))
                    for path in sorted(root.rglob('*')) if path.is_file())
    manifest = Manifest(Version('decode-cross-fixture-v1'), Provenance.FIXTURE,
                        Identity('synthetic', None),
                        (Package(package, '1', Architecture.ARM64, 'installed-cross-toolchain-fixture', None),),
                        entries)
    metadata = base / 'manifest.json'
    metadata.write_bytes(encode_manifest(manifest))
    return root, metadata


def make_overlay(base: Path) -> Path:
    """Use public declaration headers and AArch64 symbol stubs, not captured target libraries."""
    overlay = base / 'overlay'
    modules = ('gstreamer-1.0', 'gstreamer-app-1.0', 'gstreamer-video-1.0')
    include_flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags-only-I', *modules], text=True))
    flags: list[str] = []
    for index, flag in enumerate(include_flags):
        origin = Path(flag.removeprefix('-I'))
        # The multiarch system-header directory is supplied only by the cross sysroot.
        if origin == Path('/usr/include/x86_64-linux-gnu'):
            continue
        relative = f'usr/include/decode-fixture-{index}'
        shutil.copytree(origin, overlay / relative)
        flags.append(f'-I/{relative}')
    libraries = shlex.split(subprocess.check_output(
        ['pkg-config', '--libs-only-l', *modules], text=True))
    search = [Path(flag.removeprefix('-L')) for flag in shlex.split(subprocess.check_output(
        ['pkg-config', '--libs-only-L', *modules], text=True))]
    libdir = Path(subprocess.check_output(['pkg-config', '--variable=libdir', 'gstreamer-1.0'], text=True).strip())
    symbols: dict[str, str] = {}
    for library in libraries:
        name = f'lib{library.removeprefix("-l")}.so'
        origin = next(directory / name for directory in (*search, libdir) if (directory / name).is_file())
        listing = subprocess.check_output(['nm', '-D', '--defined-only', str(origin)], text=True)
        for line in listing.splitlines():
            fields = line.split()
            symbol = fields[-1].split('@', 1)[0]
            if re.fullmatch(r'_?(?:gst|g)_[A-Za-z0-9_]+', symbol):
                symbols[symbol] = fields[-2]
    source = base / 'decode-fixture-stubs.c'
    source.write_text(''.join(f'unsigned char {name}[128];\n' if kind.upper() in 'BDRSGVC'
                              else f'void {name}(void) {{}}\n'
                              for name, kind in sorted(symbols.items())))
    directory = overlay / 'usr/lib'
    (directory / 'pkgconfig').mkdir(parents=True)
    subprocess.run(['aarch64-linux-gnu-gcc', '-shared', '-nostdlib', '-fPIC',
                    '-Wl,-soname,libdecode_fixture.so', str(source),
                    '-o', str(directory / 'libdecode_fixture.so')], check=True)
    metadata = ('prefix=/usr\nlibdir=${prefix}/lib\nName: synthetic-decode-overlay\n'
                'Description: compile/link fixture only; never execute\nVersion: 1.0\n'
                'Libs: -L${libdir} -ldecode_fixture\nCflags: ' + ' '.join(flags) + '\n')
    for module in modules:
        (directory / 'pkgconfig' / f'{module}.pc').write_text(metadata)
    return overlay
