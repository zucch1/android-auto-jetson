#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Architecture boundary scenarios (task 14).

Run: python3 tests/architecture/scenarios.py ROOT CASE

Each negative case writes a REAL temporary source file containing a forbidden
include (with the whitespace shape named by the case) and requires
tests/architecture/boundary_scan.py to reject it. Nothing here asserts on a
checked-in fixture: the happy case scans the actual project module files and
proves it inspected a real, non-empty inventory, and every failure case proves
the scanner fails closed on fresh source.
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCANNER = Path(__file__).resolve().parent / "boundary_scan.py"

# Floor for the real inventory: proves the happy scan is not vacuous.
MIN_INCLUDE_FILES = 15
MIN_SRC_FILES = 5


def run_scanner(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(SCANNER), str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


def fixture_root(case: str) -> Path:
    return Path(tempfile.mkdtemp(prefix=f"architecture-{case}-"))


def write_source(root: Path, rel: str, body: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")


def expect_violation(root: Path, family: str, case: str) -> int:
    result = run_scanner(root)
    if result.returncode != 1:
        print(f"{case}: expected scanner exit 1, got {result.returncode}\n{result.stdout}")
        return 1
    if f"forbidden[{family}]" not in result.stdout:
        print(f"{case}: expected forbidden[{family}] in output\n{result.stdout}")
        return 1
    return 0


def expect_clean(root: Path, case: str) -> int:
    result = run_scanner(root)
    if result.returncode != 0:
        print(f"{case}: expected scanner exit 0, got {result.returncode}\n{result.stdout}")
        return 1
    return 0


def case_project_scan(root: Path) -> int:
    # Given: the actual worktree module trees.
    # When: the scanner runs over them.
    result = run_scanner(root)
    # Then: they are clean and the scan really inspected both trees.
    if result.returncode != 0:
        print(f"project-scan: expected exit 0, got {result.returncode}\n{result.stdout}")
        return 1
    summary = re.search(r"violations=(\d+) include=(\d+) src=(\d+)", result.stdout)
    if not summary:
        print(f"project-scan: missing summary line\n{result.stdout}")
        return 1
    violations, include_count, src_count = (int(v) for v in summary.groups())
    if violations != 0:
        print(f"project-scan: unexpected violations\n{result.stdout}")
        return 1
    if include_count < MIN_INCLUDE_FILES or src_count < MIN_SRC_FILES:
        print(
            f"project-scan: vacuous inventory include={include_count} src={src_count} "
            f"(need >= {MIN_INCLUDE_FILES} / {MIN_SRC_FILES})"
        )
        return 1
    return 0


NEGATIVE_CASES: dict[str, tuple[str, str]] = {
    # case: (family, source body with a real forbidden include)
    "neg-qt": (
        "qt",
        "#pragma once\n#  include  <QtCore/QString>\n",
    ),
    "neg-gstreamer": (
        "gstreamer",
        "#pragma once\n#include <gst/gst.h>\n",
    ),
    "neg-bluez": (
        "bluez",
        "#pragma once\n#\tinclude\t<bluetooth/bluetooth.h>\n",
    ),
    "neg-networkmanager": (
        "networkmanager",
        "#pragma once\n# include <NetworkManager.h>\n",
    ),
    "neg-aasdk": (
        "aasdk",
        "#pragma once\n#include <aasdk/SomeType.hpp>\n",
    ),
    "neg-protobuf": (
        "protobuf",
        "#include<google/protobuf/message.h>\n",
    ),
    "neg-openssl": (
        "openssl",
        "#pragma once\n# include <openssl/ssl.h>\n",
    ),
    "neg-whitespace": (
        "qt",
        "#pragma once\n#\t\tinclude\t\t<QObject>\n",
    ),
    "neg-qt-quoted": (
        "qt",
        '#pragma once\n#include "QtCore/QObject"\n',
    ),
    "neg-gstreamer-quoted": (
        "gstreamer",
        '#pragma once\n#include "gst/gst.h"\n',
    ),
    "neg-tls-quoted": (
        "aa_tls",
        '#pragma once\n#include "aa/tls/Policy.hpp"\n',
    ),
}

# Path shape per negative case: public headers for families, sources for the
# protobuf/no-space variant.
NEGATIVE_PATHS = {
    "neg-qt": "include/aa/protocol/Screen.hpp",
    "neg-gstreamer": "include/aa/audio/Pipeline.hpp",
    "neg-bluez": "include/aa/helper/Client.hpp",
    "neg-networkmanager": "include/aa/helper/Network.hpp",
    "neg-aasdk": "include/aa/protocol/Link.hpp",
    "neg-protobuf": "src/session/Codec.cpp",
    "neg-openssl": "include/aa/transport/Transport.hpp",
    "neg-whitespace": "include/aa/session/Session.hpp",
    "neg-qt-quoted": "include/aa/protocol/Screen.hpp",
    "neg-gstreamer-quoted": "include/aa/audio/Pipeline.hpp",
    "neg-tls-quoted": "include/aa/protocol/Session.hpp",
}


def make_negative(case: str) -> int:
    # Given: a fresh temporary source tree.
    family, body = NEGATIVE_CASES[case]
    root = fixture_root(case)
    write_source(root, NEGATIVE_PATHS[case], body)
    # When/Then: the scanner rejects the real temporary source by family.
    return expect_violation(root, family, case)


def case_neg_tls_leak(root: Path) -> int:
    # Given: a protocol public header that reaches OpenSSL through aa/tls.
    fixture = fixture_root("neg-tls-leak")
    write_source(
        fixture,
        "include/aa/protocol/Session.hpp",
        "#pragma once\n#  include  <aa/tls/Policy.hpp>\n",
    )
    # When/Then: the confinement rule rejects the leak.
    return expect_violation(fixture, "aa_tls", "neg-tls-leak")


def case_neg_src_outside_adapter(root: Path) -> int:
    # Given: a non-adapter source pulling an external library directly.
    fixture = fixture_root("neg-src-outside-adapter")
    write_source(
        fixture,
        "src/protocol/Codec.cpp",
        "#include <aasdk/SomeType.hpp>\n",
    )
    # When/Then: external includes outside the adapter seam are rejected.
    return expect_violation(fixture, "aasdk", "neg-src-outside-adapter")


def case_neg_quoted_after_comment(root: Path) -> int:
    # Given: quoted includes behind preceding comments (earlier line and same line).
    fixture = fixture_root("neg-quoted-after-comment")
    write_source(
        fixture,
        "include/aa/helper/Client.hpp",
        "#pragma once\n"
        "// documented dependency\n"
        '#include "gst/gst.h"\n'
        '/* note */ #include "QtCore/QObject"\n',
    )
    # When: the scanner runs over the fixture.
    result = run_scanner(fixture)
    # Then: each comment-preceded quoted include is still reported by family.
    if result.returncode != 1:
        print(f"neg-quoted-after-comment: expected exit 1, got {result.returncode}\n{result.stdout}")
        return 1
    for family in ("gstreamer", "qt"):
        if f"forbidden[{family}]" not in result.stdout:
            print(f"neg-quoted-after-comment: expected forbidden[{family}]\n{result.stdout}")
            return 1
    return 0


def case_allow_tls_legacy(root: Path) -> int:
    # Given: the legacy task-8 TLS module header (documented OpenSSL exception).
    fixture = fixture_root("allow-tls-legacy")
    write_source(
        fixture,
        "include/aa/tls/Creds.hpp",
        "#pragma once\n#include <aa/tls/Policy.hpp>\n#include <openssl/ssl.h>\n",
    )
    # When/Then: the TLS module itself is allowed to expose OpenSSL.
    return expect_clean(fixture, "allow-tls-legacy")


def case_allow_src_adapter(root: Path) -> int:
    # Given: a private adapter directory (the designated external seam).
    fixture = fixture_root("allow-src-adapter")
    write_source(
        fixture,
        "src/protocol/aasdk_adapter/Codec.cpp",
        "#include <aasdk/SomeType.hpp>\n"
        "#include <google/protobuf/message.h>\n"
        "# include <openssl/ssl.h>\n",
    )
    # When/Then: private adapters may use external implementations.
    return expect_clean(fixture, "allow-src-adapter")


def case_allow_commented_include(root: Path) -> int:
    # Given: include spellings that are only comments or string literals.
    fixture = fixture_root("allow-commented-include")
    write_source(
        fixture,
        "include/aa/core/Note.hpp",
        "#pragma once\n"
        "// #include <gst/gst.h>\n"
        "/* #include <QML> */\n"
        '// #include "gst/gst.h"\n'
        '/* #include "QtCore/QObject" */\n'
        'inline constexpr const char* kBad = "#include <QtCore/QString>";\n'
        'inline constexpr const char* kWorse = "#include \\"aa/tls/Policy.hpp\\"";\n',
    )
    # When/Then: they are not real includes and must not fail the scan.
    return expect_clean(fixture, "allow-commented-include")


CASES = {
    "project-scan": case_project_scan,
    "neg-tls-leak": case_neg_tls_leak,
    "neg-src-outside-adapter": case_neg_src_outside_adapter,
    "neg-quoted-after-comment": case_neg_quoted_after_comment,
    "allow-tls-legacy": case_allow_tls_legacy,
    "allow-src-adapter": case_allow_src_adapter,
    "allow-commented-include": case_allow_commented_include,
}


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} ROOT CASE", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    if case in NEGATIVE_CASES:
        return make_negative(case)
    if case in CASES:
        return CASES[case](root)
    print(f"unknown case: {case}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
