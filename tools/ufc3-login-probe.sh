#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
mode=(-batch)
if [[ ${BB_LOGIN_INTERACTIVE:-0} == 1 ]]; then mode=(); fi
exec gdb -q "${mode[@]}" -x "$repo/tools/ufc3-login.gdb" --args "$repo/out/sacredfire" "$@"
