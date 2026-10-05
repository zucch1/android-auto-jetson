#!/usr/bin/env bash
set -euo pipefail
# Offline only. Invoke with bash; all three paths are explicit CLI arguments.
script_dir="$(dirname -- "${BASH_SOURCE[0]}")"
exec python3 -B "${script_dir}/cli.py" materialize "$@"
