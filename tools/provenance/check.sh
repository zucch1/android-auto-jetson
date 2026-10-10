#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
exec "${PYTHON:-python3}" -B "$HERE/check.py" "$@"
