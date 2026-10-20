# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/qualification/check_privacy.py PACKAGE_DIRECTORY
"""Conservative export checks; not universal secret detection or anonymization."""
from __future__ import annotations

import argparse
import json
import re
import stat
from pathlib import Path
from typing import Final

from common import EvidenceError, package_file, require, unique_pairs

TEXT_SUFFIXES: Final = {'.json', '.jsonl', '.ndjson', '.txt', '.log', '.md'}
CAPTURE_NAME: Final = re.compile(
    r'(?i)(?:capture|usbmon|snoop|dump|credential|id_rsa|id_ed25519)|'
    r'\.(?:pcapng?|cap|h264|264|aac|wav|pcm|mp4|bin|key|pem|crt|p12|zip|gz|tar)$')
MAC: Final = re.compile(r'(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])')
SENSITIVE: Final = re.compile(
    r'''(?ix)["']?\b(?:usb[_ -]?serial|serial(?:[_ -]?number|no)?|iSerial|'''
    r'''android_id|imei|imsi|mac(?:_address)?|bssid|ssid|latitude|longitude|'''
    r'''phone[_ -]?id|identity|device[_ -]?id|subscriber[_ -]?id|meid|udid|uuid|guid|'''
    r'''location|gps|password|passwd|psk|(?:access_|refresh_)?token|'''
    r'''api[_-]?key|private[_-]?key|(?:client_)?secret|authorization)'''
    r'''["']?\s*[:=][ \t]*([^\r\n]*)''')
SAFE_VALUE: Final = re.compile(
    r'''(?P<quote>["']?)(?:\[REDACTED\]|redacted|null|sha256:[0-9a-f]{16,64}|'''
    r'''id-[0-9a-f]{16})(?P=quote)\Z''')
RAW_CONTENT: Final = re.compile(
    r'(?i)-----BEGIN [^-]*(?:PRIVATE KEY|CERTIFICATE)-----|'
    r'\b(?:Bearer|Basic)\s+[A-Za-z0-9+/=._-]+|'
    r'aa-projection-perf-capture/|\bUSBPcap\b|\bAuthorization\s*:|'
    r'\biSerial\s+\d+\s+(?!\[REDACTED\]|redacted|sha256:)[^\s]+|'
    r'\b(?:https?|ssh)://[^\s/:]+:[^\s/@]+@|'
    r'\b[+-]?\d{1,2}\.\d{4,}\s*,\s*[+-]?\d{1,3}\.\d{4,}')
MAGIC: Final = (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4', b'\x0a\x0d\x0d\x0a',
               b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d', b'\x00\x00\x00\x01')


def scan(root: Path) -> None:
    """Inspect every package file, including unreferenced files; never echo content."""
    require(root.is_dir() and not root.is_symlink(), 'INVALID_PACKAGE_DIRECTORY')
    for entry in sorted(root.rglob('*')):
        require(not entry.is_symlink(), 'PRIVACY_SYMLINK')
        if stat.S_ISDIR(entry.stat().st_mode):
            continue
        relative = entry.relative_to(root).as_posix()
        path = package_file(root, relative)
        require(not MAC.search(relative), 'PRIVACY_RAW_MAC_FILENAME')
        require(not CAPTURE_NAME.search(relative), 'PRIVACY_FORBIDDEN_FILE')
        require(path.suffix.lower() in TEXT_SUFFIXES, 'PRIVACY_UNSUPPORTED_FILE')
        data = path.read_bytes()
        require(not data.startswith(MAGIC) and b'\0' not in data, 'PRIVACY_BINARY_MATERIAL')
        text = data.decode('utf-8')
        # Decode JSON escapes so an identifier cannot hide as \\u003a, etc.
        is_json = path.suffix.lower() in {'.json', '.jsonl', '.ndjson'}
        if is_json:
            documents = [text] if path.suffix.lower() == '.json' else text.splitlines()
            # One field per line preserves the whole value, including quoted tails.
            text = '\n'.join(json.dumps(json.loads(line, object_pairs_hook=unique_pairs),
                                        ensure_ascii=False, indent=2)
                             for line in documents if line.strip())
        require(not MAC.search(text), 'PRIVACY_RAW_MAC')
        require(not RAW_CONTENT.search(text), 'PRIVACY_RAW_MATERIAL')
        for match in SENSITIVE.finditer(text):
            value = match.group(1).strip()
            if is_json:
                value = value.removesuffix(',')
            require(SAFE_VALUE.fullmatch(value) is not None, 'PRIVACY_SENSITIVE_VALUE')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    args = parser.parse_args()
    try:
        scan(args.package)
    except (EvidenceError, OSError, UnicodeError, ValueError, RecursionError):
        print('privacy-check: FAIL unsafe or unreadable package (content withheld)')
        return 1
    print('privacy-check: PASS known export checks only; manual review required')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
