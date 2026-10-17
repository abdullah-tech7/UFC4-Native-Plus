#!/usr/bin/env bash
# UFC 3: rebuild, run with a timeout and print where the run stopped.
cd -- "$(dirname -- "$0")/.."
n=$(ls out/run*.log 2>/dev/null | wc -l); log=out/run$((n+1)).log
bash build.sh > build.log 2>&1 || { grep -E 'error' build.log | head; exit 1; }
BB_TIMEOUT=${BB_TIMEOUT:-90} timeout ${ITER_TIMEOUT:-150} bash run.sh > "$log" 2>&1; rc=$?
echo "log=$log exit=$rc threads=$(grep -c 'guest thread .* created' "$log") frames=$(grep -ciE 'present|flip' "$log")"
grep -E 'STOP|API:|Caller' "$log" | head -4
