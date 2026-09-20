#!/bin/sh
# smoke tests for motiris - no network, no API key needed (echo transport).
set -e
cd "$(dirname "$0")/.."

make motiris examples/hello_plugin.so >/dev/null 2>&1

echo "== 1: agent loop runs end-to-end (echo transport)"
OUT=$(./motiris -p hi --transport echo)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: $OUT"; exit 1; }

echo "== 2: runtime plugin loads and registers a tool"
PDIR=$(mktemp -d)
cp examples/hello_plugin.so "$PDIR/"
ERR=$(./motiris -p hi --transport echo --plugin-dir "$PDIR" 2>&1)
rm -rf "$PDIR"
echo "$ERR" | grep -q "tool 'hello' registered" || { echo "FAIL: $ERR"; exit 1; }

echo "== 3: unknown tool tolerated when built-ins disabled"
OUT=$(./motiris -p hi --transport echo --no-tools)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: $OUT"; exit 1; }

echo "== 4: binary stays tiny"
SIZE=$(stat -c %s motiris)
[ "$SIZE" -lt 131072 ] || { echo "FAIL: size=$SIZE"; exit 1; }
echo "   motiris size: $SIZE bytes"

echo "== 5: missing prompt -> clean error"
./motiris --max-steps 1 </dev/null 2>&1 | grep -q 'no prompt' || { echo FAIL; exit 1; }

echo "== 6: session save/resume round-trip"
F=$(mktemp)
./motiris -p "first user msg" --transport echo --save "$F" >/dev/null
grep -q '^Ufirst user msg$' "$F" || { echo "FAIL: save"; cat "$F"; exit 1; }
OUT=$(./motiris -r "$F" --transport echo)
echo "$OUT" | grep -q 'echo round complete' || { echo "FAIL: resume"; exit 1; }
rm -f "$F"

echo "== 7: env config honored (MOTIRIS_MODEL)"
OUT=$(MOTIRIS_MODEL=foo-model ./motiris -p hi --transport echo -v 2>&1)
echo "$OUT" | grep -q 'foo-model via' || { echo FAIL; exit 1; }

echo "== 8: provider fallback (dead -> mock)"
TH8=$(mktemp -d)
printf '{"providers":[{"name":"dead","model":"p1","base_url":"http://127.0.0.1:1/v1/chat/completions"},{"name":"mock","model":"p2","base_url":"http://127.0.0.1:18099/v1/chat/completions"}]}' > "$TH8/config.json"
python3 -c '
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length",0)))
        b=json.dumps({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"fallback ok"}}]}).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18099),H).serve_forever()
' &
MOCKPID=$!
sleep 0.5
OUT=$(MOTIRIS_HOME="$TH8" ./motiris -p hi --transport libcurl --max-steps 1 -v 2>&1)
kill $MOCKPID 2>/dev/null
rm -rf "$TH8"
echo "$OUT" | grep -q 'fallback ok' || { echo "FAIL: $OUT"; exit 1; }
echo "$OUT" | grep -q 'provider dead failed' || { echo "FAIL: dead not attempted"; exit 1; }

echo "== 9: gateway HTTP service + auth"
GW=$(mktemp -d)
printf '{"transport":"echo","tools":false}' > "$GW/config.json"
MOTIRIS_HOME="$GW" ./motiris --gateway 127.0.0.1:18089 --no-tools >/dev/null 2>&1 &
GWPID=$!
sleep 0.3
H=$(curl -s http://127.0.0.1:18089/health)
echo "$H" | grep -q '"status":"ok"' || { echo "FAIL health: $H"; kill $GWPID 2>/dev/null; exit 1; }
R=$(curl -s -X POST http://127.0.0.1:18089/v1/chat -d '{"chat_id":"g1","message":"hi"}')
echo "$R" | grep -q 'echo round complete' || { echo "FAIL chat: $R"; kill $GWPID 2>/dev/null; exit 1; }
kill $GWPID 2>/dev/null
wait $GWPID 2>/dev/null || true
MOTIRIS_GATEWAY_TOKEN=sekret MOTIRIS_HOME="$GW" ./motiris --gateway 127.0.0.1:18090 --no-tools >/dev/null 2>&1 &
GWPID2=$!
sleep 0.3
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18090/health || true)
[ "$CODE" = "401" ] || { echo "FAIL auth: got $CODE"; kill $GWPID2 2>/dev/null; exit 1; }
kill $GWPID2 2>/dev/null
wait $GWPID2 2>/dev/null || true
rm -rf "$GW"

echo "smoke: all tests passed"