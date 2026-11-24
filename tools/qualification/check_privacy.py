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

from common import JSON, EvidenceError, package_file, require, unique_pairs

TEXT_SUFFIXES: Final = {'.json', '.jsonl', '.ndjson', '.txt', '.log', '.md'}
CAPTURE_NAME: Final = re.compile(
    r'(?i)(?:capture|usbmon|snoop|dump|credential|id_rsa|id_ed25519)|'
    r'\.(?:pcapng?|cap|h264|264|aac|wav|pcm|mp4|bin|key|pem|crt|p12|zip|gz|tar)$')
MAC: Final = re.compile(
    r'(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])|'
    r'dev_(?:[0-9a-f]{2}_){5}[0-9a-f]{2}(?![0-9a-f])')
RAW_AP_NAME: Final = re.compile(r'(?i)(?<![0-9a-z])AA-[0-9a-f]{16}(?![0-9a-z])')
IW_SSID: Final = re.compile(r'(?m)^[ \t]*ssid[ \t]+(?![ \t]*[:=])([^\r\n]*)')
GETPROP_SERIAL: Final = re.compile(r'(?i)\[ro\.(?:boot\.)?serialno\][ \t]*:[ \t]*([^\r\n]*)')
SENSITIVE_LABEL: Final = (
    r'''(?:(?:(?:usb|phone|device|adb)[_ -]?)?serial(?:[_ -]?number|no)?|iSerial|'''
    r'''android[_ -]?id|imei|imsi|(?:(?:bt|bluetooth|wifi|peer)[_ -]?)?mac(?:[_ -]?address)?|'''
    r'''bdaddr|(?:bt|bluetooth|wifi|peer)[_ -]?addr(?:ess)?|'''
    r'''(?:(?:ap|wifi|wpa|car[_ -]?wifi|aa|hotspot)[_ -]?)?(?:bssid|ssid|psk|passphrase)|'''
    r'''pre[_ -]?shared[_ -]?key|latitude|longitude|'''
    r'''(?:phone|device|subscriber)[_ -]?(?:identifier|identity|id)|identity|meid|udid|uuid|guid|'''
    r'''location|gps|password|passwd|(?:access_|refresh_)?token|'''
    r'''api[_-]?key|private[_-]?key|(?:client_)?secret|authorization)''')
SENSITIVE_KEY: Final = re.compile(r'\b' + SENSITIVE_LABEL + r'\Z', re.IGNORECASE)
SENSITIVE: Final = re.compile(
    r'''["']?\b''' + SENSITIVE_LABEL + r'''["']?\s*[:=][ \t]*([^\r\n]*)''',
    re.IGNORECASE)
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


def check_text_values(text: str) -> None:
    # Run the raw-material patterns on the decoded string too: in json.dumps
    # output a newline or tab becomes the letters "n"/"t", which defeats the
    # word-boundary lookarounds below.
    require(not MAC.search(text), 'PRIVACY_RAW_MAC')
    require(not RAW_CONTENT.search(text) and not RAW_AP_NAME.search(text),
            'PRIVACY_RAW_MATERIAL')
    for match in SENSITIVE.finditer(text):
        require(SAFE_VALUE.fullmatch(match[1].strip()) is not None, 'PRIVACY_SENSITIVE_VALUE')
    for match in IW_SSID.finditer(text):
        require(SAFE_VALUE.fullmatch(match[1].strip()) is not None, 'PRIVACY_SENSITIVE_VALUE')
    for match in GETPROP_SERIAL.finditer(text):
        value = match[1].strip()
        unwrapped = value[1:-1] if value.startswith('[') and value.endswith(']') else value
        require(SAFE_VALUE.fullmatch(value) is not None or SAFE_VALUE.fullmatch(unwrapped) is not None,
                'PRIVACY_SENSITIVE_VALUE')


def check_json_values(value: JSON) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            # Keys are decoded text too: scan them for raw material the same way.
            check_text_values(key)
            if SENSITIVE_KEY.search(key):
                require(SAFE_VALUE.fullmatch(json.dumps(item, ensure_ascii=False)) is not None,
                        'PRIVACY_SENSITIVE_VALUE')
            check_json_values(item)
    elif isinstance(value, list):
        for item in value:
            check_json_values(item)
    elif isinstance(value, str):
        check_text_values(value)


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
        require(not RAW_AP_NAME.search(relative), 'PRIVACY_RAW_AP_NAME_FILENAME')
        require(not CAPTURE_NAME.search(relative), 'PRIVACY_FORBIDDEN_FILE')
        require(path.suffix.lower() in TEXT_SUFFIXES, 'PRIVACY_UNSUPPORTED_FILE')
        data = path.read_bytes()
        require(not data.startswith(MAGIC) and b'\0' not in data, 'PRIVACY_BINARY_MATERIAL')
        text = data.decode('utf-8')
        # Decode JSON escapes so an identifier cannot hide as \\u003a, etc.
        if path.suffix.lower() in {'.json', '.jsonl', '.ndjson'}:
            lines = [text] if path.suffix.lower() == '.json' else text.splitlines()
            documents = [json.loads(line, object_pairs_hook=unique_pairs)
                         for line in lines if line.strip()]
            for document in documents:
                check_json_values(document)
            text = '\n'.join(json.dumps(document, ensure_ascii=False, indent=2)
                             for document in documents)
        else:
            check_text_values(text)
        require(not MAC.search(text), 'PRIVACY_RAW_MAC')
        require(not RAW_CONTENT.search(text) and not RAW_AP_NAME.search(text),
                'PRIVACY_RAW_MATERIAL')


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
