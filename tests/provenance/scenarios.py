# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 tests/provenance/scenarios.py ROOT CASE
"""Exercise the public checker against private, retained mutation fixtures."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    """Given a source fixture, mutate once, then require the named failure."""
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    # Given: each invocation has a fresh private copy; fixtures are retained.
    fixture = Path(tempfile.mkdtemp(prefix=f'provenance-{case}-'))
    shutil.copytree(root / 'third_party', fixture / 'third_party')
    for name in ('THIRD_PARTY_NOTICES.md', 'PROVENANCE.md'):
        shutil.copy2(root / name, fixture / name)
    aasdk = fixture / 'third_party/aasdk'
    oaa = next((fixture / 'third_party/reference/open-android-auto').iterdir())
    proto = next(oaa.rglob('*.proto'))
    header = aasdk / 'include/aasdk/USB/USBWrapper.hpp'
    expected = ''
    # When: a single mutation through the shipped check.sh CLI.
    match case:
        case 'baseline':
            pass
        case 'overlay':
            target = aasdk / proto.relative_to(oaa)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(proto, target)
            expected = 'OAA_OVERLAY_IN_AASDK'
        case 'content':
            with (aasdk / 'Readme.md').open('ab') as output:
                output.write(b'\nmutation\n')
            expected = 'BLOB_MISMATCH'
        case 'header':
            data = header.read_bytes()
            assert b'Copyright' in data
            header.write_bytes(data.replace(b'Copyright', b'Removed', 1))
            expected = 'BLOB_MISMATCH'
        case 'extra':
            (aasdk / 'unexpected.txt').write_text('untracked\n')
            expected = 'EXTRA_VENDORED_FILE'
        case 'generated':
            (aasdk / 'injected.pb.h').write_text('// generated output\n')
            expected = 'GENERATED_OUTPUT_IN_AASDK'
        case 'license':
            (aasdk / 'LICENSE').write_text('invented upstream license\n')
            expected = 'INVENTED_LICENSE_PRESENT'
        case 'gpl':
            (fixture / 'third_party/LICENSES/GPL-3.0.txt').write_text('shortened\n')
            expected = 'GPL_TEXT_MISMATCH'
        case 'inventory':
            (fixture / 'third_party/provenance/inventory.json').write_text('{}\n')
            expected = 'INVENTORY_MISMATCH'
        case 'listing':
            (fixture / 'third_party/provenance/oaa.ls-tree').write_text('forged\n')
            expected = 'UPSTREAM_LISTING_MISMATCH'
        case 'symlink':
            (aasdk / 'linked.proto').symlink_to(proto)
            expected = 'SYMLINK_REJECTED'
        case 'missing':
            (aasdk / 'Readme.md').rename(fixture / 'preserved-Readme.md')
            expected = 'MISSING_VENDORED_FILE'
        case 'credential':
            restored = aasdk / 'cert/headunit.key'
            restored.parent.mkdir(parents=True, exist_ok=True)
            restored.write_bytes(b'not the real bytes but a restored credential name\n')
            expected = 'SANITIZED_FILE_RESTORED'
        case 'sanitized-edit':
            with (aasdk / 'src/Messenger/Cryptor.cpp').open('ab') as output:
                output.write(b'\nmutation\n')
            expected = 'SANITIZATION_MISMATCH'
        case 'mode':
            path = aasdk / 'Readme.md'
            path.chmod(path.stat().st_mode ^ 0o100)
            expected = 'MODE_MISMATCH'
        case _:
            raise AssertionError(f'Unknown test case: {case}')
    result = subprocess.run(
        [str(root / 'tools/provenance/check.sh'), '--root', str(fixture)],
        text=True, capture_output=True, check=False,
    )
    # Then: observable CLI exit and diagnostic, not implementation internals.
    print(f'case={case} checker_exit={result.returncode} fixture={fixture}')
    print(result.stdout, end='')
    print(result.stderr, end='')
    if expected:
        assert result.returncode == 1, result
        assert expected in result.stdout, result
    else:
        assert result.returncode == 0, result
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
