# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/sysroot/model.py
"""Typed boundary rejection tests; no target observations are manufactured."""
from __future__ import annotations

from dataclasses import FrozenInstanceError
import hashlib
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.sysroot.manifest import encode_manifest, parse_manifest
from tools.sysroot.models import Code, SysrootError, require_observed
from fixtures import Json, create


def rejected(raw: bytes, code: Code) -> None:
    # When: parse one boundary input.
    try:
        parse_manifest(raw)
    except SysrootError as error:
        # Then: assert the typed category, never diagnostic prose.
        assert error.code == code, error
    else:
        raise AssertionError(code)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix='sysroot-model-') as temporary:
        fixture = create(Path(__file__).resolve().parents[2], Path(temporary))
        # Given: explicit fixture metadata with opaque synthetic kernel identity.
        model = parse_manifest(fixture.metadata.read_bytes())
        assert parse_manifest(encode_manifest(model)) == model
        try:
            setattr(model, 'version', model.version)
        except FrozenInstanceError:
            print('PASS frozen-model')
        else:
            raise AssertionError('mutable model')
        try:
            require_observed(model)
        except SysrootError as error:
            assert error.code == Code.FIXTURE_PROVENANCE
        else:
            raise AssertionError('fixture passed guard')
        mutations: tuple[tuple[str, Json, Code], ...] = (
            ('version', '', Code.MALFORMED_METADATA),
            ('version', '../v1', Code.UNSAFE_PATH),
            ('version', 'v/1', Code.UNSAFE_PATH),
            ('provenance', 'authentic', Code.MALFORMED_METADATA),
            ('capability', 'qualified', Code.MALFORMED_METADATA),
            ('identity', {'kernel': 1, 'l4t': None}, Code.MALFORMED_METADATA),
            ('files', [], Code.MALFORMED_METADATA),
        )
        for field, value, code in mutations:
            document = fixture.document()
            document[field] = value
            rejected(json.dumps(document).encode(), code)
            print(f'PASS malformed-{field}')
        for arch, justification, accepted in (
                ('all', None, False), ('all', 'architecture-independent data', True),
                ('amd64', None, False), ('aarch64', None, False)):
            document = fixture.document()
            packages = document['packages']
            assert isinstance(packages, list) and isinstance(packages[0], dict)
            packages[0]['architecture'] = arch
            packages[0]['all_justification'] = justification
            raw = json.dumps(document).encode()
            if accepted:
                parsed = parse_manifest(raw)
                assert parsed.packages[0].all_justification == justification
            else:
                rejected(raw, Code.MALFORMED_METADATA)
            print(f'PASS architecture-{arch}-{accepted}')
        for link in ('/usr/share/data', 'unknown', 'alias', 'data/../data', '../../../usr/share/data'):
            document = fixture.document()
            files = document['files']
            assert isinstance(files, list) and isinstance(files[1], dict)
            files[1]['link_text'] = link
            files[1]['sha256'] = hashlib.sha256(link.encode()).hexdigest()
            rejected(json.dumps(document).encode(), Code.SYMLINK_CLOSURE)
            print(f'PASS link-{link}')
        raw = fixture.metadata.read_bytes().replace(b'"version": "v1"', b'"version": "v1", "version": "v2"')
        rejected(raw, Code.DUPLICATE)
        print('PASS duplicate-json-key')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
