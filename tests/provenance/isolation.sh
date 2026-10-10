#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ROOT=$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")
for scenario in baseline overlay content header extra generated license gpl inventory listing symlink missing; do
    "${PYTHON:-python3}" -B "$ROOT/tests/provenance/scenarios.py" "$ROOT" "$scenario"
done
