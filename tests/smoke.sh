#!/bin/sh
# smoke tests for hermote - no network, no API key needed (echo transport).
set -e
cd "$(dirname "$0")/.."

make hermote examples/hello_plugin.so >/dev/null 2>&1

echo "== 1: agent loop runs end-to-end (echo transport)"
OUT=$(./hermote -p hi --transport echo)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: $OUT"; exit 1; }

echo "== 2: runtime plugin loads and registers a tool"
PDIR=$(mktemp -d)
cp examples/hello_plugin.so "$PDIR/"
ERR=$(./hermote -p hi --transport echo --plugin-dir "$PDIR" 2>&1)
rm -rf "$PDIR"
echo "$ERR" | grep -q "tool 'hello' registered" || { echo "FAIL: $ERR"; exit 1; }

echo "== 3: unknown tool tolerated when built-ins disabled"
OUT=$(./hermote -p hi --transport echo --no-tools)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: $OUT"; exit 1; }

echo "== 4: binary stays tiny"
SIZE=$(stat -c %s hermote)
[ "$SIZE" -lt 131072 ] || { echo "FAIL: size=$SIZE"; exit 1; }
echo "   hermote size: $SIZE bytes"

echo "== 5: missing prompt -> clean error"
./hermote --max-steps 1 </dev/null 2>&1 | grep -q 'no prompt' || { echo FAIL; exit 1; }

echo "== 6: session save/resume round-trip"
F=$(mktemp)
./hermote -p "first user msg" --transport echo --save "$F" >/dev/null
grep -q '^Ufirst user msg$' "$F" || { echo "FAIL: save"; cat "$F"; exit 1; }
OUT=$(./hermote -r "$F" --transport echo)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: resume"; exit 1; }
rm -f "$F"

echo "smoke: all tests passed"