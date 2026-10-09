#!/usr/bin/env bash
set -euo pipefail
HERE=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
exec "${PYTHON:-python3}" -B "$HERE/check.py" "$@"
