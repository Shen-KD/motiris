#!/bin/sh
# perf.sh - resource & performance guard for motiris (CI job `perf`).
#
# Measures and asserts three things on the real binary:
#   1. cold-start latency (echo transport, plugins off)
#   2. peak RSS (same run; includes libcurl + tool registry)
#   3. agent-loop overhead across N steps (echo, tools on)
#
# Thresholds are intentionally loose (CI runners vary 2-3x); they catch
# regressions like accidental unbounded buffers or startup bloat, not
# micro-optimizations. Fail = CI red.
set -e
cd "$(dirname "$0")/.."

if ! command -v /usr/bin/time >/dev/null 2>&1; then
  echo "perf: /usr/bin/time not installed, skipping"
  exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ------------- 1+2. cold-start latency & peak RSS -------------
echo "== perf: cold start (echo, no plugins), 3 runs =="
: > "$TMP/start"
for i in 1 2 3; do
  /usr/bin/time -f "%e %M" -o "$TMP/start" -a \
    ./motiris -p hi --transport echo --no-plugin > /dev/null
done
# avg wall seconds, max RSS KB
AVG_MS=$(awk '{ sum += $1 } END { printf "%.0f", sum / NR * 1000 }' "$TMP/start")
MAX_RSS=$(awk 'BEGIN { m = 0 } { if ($2 > m) m = $2 } END { print m }' "$TMP/start")
cat "$TMP/start" | sed 's/^/  /'
echo "  -> avg start ${AVG_MS}ms, peak RSS ${MAX_RSS}KB"

# ------------- 3. agent loop over N steps -------------
echo "== perf: agent loop, 5 steps, full toolset (echo) =="
/usr/bin/time -f "%e %M" -o "$TMP/loop" \
  ./motiris -p hi --transport echo --max-steps 5 > /dev/null
LOOP_S=$(awk '{ print $1 }' "$TMP/loop")
LOOP_RSS=$(awk '{ print $2 }' "$TMP/loop")
sed 's/^/  /' "$TMP/loop"
echo "  -> 5-step loop ${LOOP_S}s, peak RSS ${LOOP_RSS}KB"

# ------------- assertions -------------
FAIL=0
[ "$AVG_MS" -lt 500 ]  || { echo "FAIL: cold start ${AVG_MS}ms >= 500ms threshold"; FAIL=1; }
[ "$MAX_RSS" -lt 65536 ] || { echo "FAIL: peak RSS ${MAX_RSS}KB >= 64MB threshold"; FAIL=1; }
[ "$LOOP_RSS" -lt 65536 ] || { echo "FAIL: loop RSS ${LOOP_RSS}KB >= 64MB"; FAIL=1; }
LOOP_MS=$(awk "BEGIN { printf \"%.0f\", $LOOP_S * 1000 }")
[ "$LOOP_MS" -lt 3000 ] || { echo "FAIL: 5-step loop ${LOOP_S}s >= 3s"; FAIL=1; }

if [ "$FAIL" = 0 ]; then
  echo "perf: all thresholds OK"
  exit 0
fi
exit 1