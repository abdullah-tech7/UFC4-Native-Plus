#!/usr/bin/env bash
# BB_PROBE wrapper: run the game under gdb with tools/ufc3-ui-probe.gdb (pregame probe).
# Pick the breakpoints with UFC3_PROBE_ROLE=host|client (default: client).
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
exec gdb -q -batch -x "$repo/tools/ufc3-ui-probe.gdb" --args "$repo/out/sacredfire" "$@"
