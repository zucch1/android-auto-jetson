#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(dirname "$(realpath "$0")")"
exec python3 -B "$here/wireless_probe.py" "$@"
