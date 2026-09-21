#!/bin/sh
# smoke tests for motiris - no network, no API key needed (echo transport).
set -e
cd "$(dirname "$0")/.."

# kill any background jobs (mock servers) on any exit path, so failed
# runs never leave orphan listeners holding test ports. dash's jobs -p
# is unreliable on EXIT, so collect PIDs explicitly.
MOCK_PIDS=""
trap 'for p in $MOCK_PIDS; do kill "$p" 2>/dev/null || true; done; true' EXIT

# isolate from any developer MOTIRIS_* env that would leak into tests
# (shell-exported values intentionally override config.json)
unset MOTIRIS_API_KEY MOTIRIS_MODEL MOTIRIS_BASE_URL MOTIRIS_PLUGIN_DIR \
      MOTIRIS_GATEWAY_TOKEN MOTIRIS_WORKSPACE 2>/dev/null || true

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
[ "$SIZE" -lt 163840 ] || { echo "FAIL: size=$SIZE"; exit 1; }
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
MOCK_PIDS="$MOCK_PIDS $MOCKPID"
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
MOCK_PIDS="$MOCK_PIDS $GWPID"
sleep 0.3
H=$(curl -s http://127.0.0.1:18089/health)
echo "$H" | grep -q '"status":"ok"' || { echo "FAIL health: $H"; kill $GWPID 2>/dev/null; exit 1; }
R=$(curl -s -X POST http://127.0.0.1:18089/v1/chat -d '{"chat_id":"g1","message":"hi"}')
echo "$R" | grep -q 'echo round complete' || { echo "FAIL chat: $R"; kill $GWPID 2>/dev/null; exit 1; }
kill $GWPID 2>/dev/null
wait $GWPID 2>/dev/null || true
MOTIRIS_GATEWAY_TOKEN=sekret MOTIRIS_HOME="$GW" ./motiris --gateway 127.0.0.1:18090 --no-tools >/dev/null 2>&1 &
GWPID2=$!
MOCK_PIDS="$MOCK_PIDS $GWPID2"
sleep 0.3
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18090/health || true)
[ "$CODE" = "401" ] || { echo "FAIL auth: got $CODE"; kill $GWPID2 2>/dev/null; exit 1; }
kill $GWPID2 2>/dev/null
wait $GWPID2 2>/dev/null || true
rm -rf "$GW"

echo "== 10: file tools via agent loop (read ok + workspace guard)"
F10=$(mktemp -d)
mkdir -p "$F10/ws" "$F10/h1" "$F10/h2"
printf 'hello content\n' > "$F10/ws/hello.txt"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18091/v1/chat/completions"}' > "$F10/h1/config.json"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18092/v1/chat/completions"}' > "$F10/h2/config.json"
python3 -c '
import http.server, json, threading
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            c = str(msgs[-1].get("content",""))[:150]
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"R:"+c}}]}
        else:
            p = "/etc/passwd" if self.server.server_address[1] == 18092 else "hello.txt"
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_f1","type":"function","function":{"name":"read_file","arguments":json.dumps({"path":p})}}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
for port in (18091,18092):
    s = http.server.HTTPServer(("127.0.0.1",port),H); threading.Thread(target=s.serve_forever,daemon=True).start()
threading.Event().wait()
' &
MOCKPID=$!
MOCK_PIDS="$MOCK_PIDS $MOCKPID"
sleep 0.5
ROOT=$(pwd)
cd "$F10/ws"
O1=$(MOTIRIS_HOME="$F10/h1" MOTIRIS_WORKSPACE="$F10/ws" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O1" | grep -q 'hello content' || { echo "FAIL read: $O1"; exit 1; }
O2=$(MOTIRIS_HOME="$F10/h2" MOTIRIS_WORKSPACE="$F10/ws" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O2" | grep -q 'outside workspace' || { echo "FAIL guard: $O2"; exit 1; }
kill $MOCKPID 2>/dev/null
wait $MOCKPID 2>/dev/null || true
cd "$ROOT"
rm -rf "$F10"

echo "== 11: web_fetch + memory + shell deny"
F11=$(mktemp -d)
mkdir -p "$F11/ws" "$F11/h3" "$F11/h4"
printf '<html><body><h1>Hi there</h1><script>x</script>web ok</body></html>' > "$F11/ws/page.html"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18093/v1/chat/completions","shell_deny":"rm"}' > "$F11/h3/config.json"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18094/v1/chat/completions"}' > "$F11/h4/config.json"
python3 -c '
import http.server, json, threading, urllib.parse
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        b=b"<html><body><h1>Hi there</h1><script>x</script>web ok</body></html>"
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            c = str(msgs[-1].get("content",""))[:120]
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"R:"+c}}]}
        else:
            port = self.server.server_address[1]
            if port == 18093:
                tc = {"name":"shell","arguments":json.dumps({"command":"rm /tmp/zz"})}
            else:
                tc = {"name":"memory_set","arguments":json.dumps({"key":"planet","value":"mars"})}
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_w1","type":"function","function":tc}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
for port in (18093,18094):
    s = http.server.HTTPServer(("127.0.0.1",port),H); threading.Thread(target=s.serve_forever,daemon=True).start()
threading.Event().wait()
' &
M11PID=$!
MOCK_PIDS="$MOCK_PIDS $M11PID"
sleep 0.5
ROOT=$(pwd)
cd "$F11/ws"
O3=$(MOTIRIS_HOME="$F11/h3" MOTIRIS_WORKSPACE="$F11/ws" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O3" | grep -q 'blocked by shell policy' || { echo "FAIL deny: $O3"; kill $M11PID 2>/dev/null; exit 1; }
O4=$(MOTIRIS_HOME="$F11/h4" MOTIRIS_WORKSPACE="$F11/ws" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O4" | grep -q 'stored' || { echo "FAIL memory: $O4"; kill $M11PID 2>/dev/null; exit 1; }
# memory persisted file exists
ls "$HOME/.local/share/motiris/memory.json" >/dev/null 2>&1 || { echo "FAIL memory file"; kill $M11PID 2>/dev/null; exit 1; }
kill $M11PID 2>/dev/null
wait $M11PID 2>/dev/null || true
cd "$ROOT"
rm -rf "$F11"

echo "== 12: cron --once + subagent spawn"
F12=$(mktemp -d)
mkdir -p "$F12/h"
printf '{"transport":"echo"}' > "$F12/h/config.json"
cat > "$F12/jobs.json" <<'JEOF'
[{"id":"j1","interval_s":1,"prompt":"cron hello"}]
JEOF
CR=$(MOTIRIS_HOME="$F12/h" "$ROOT/motiris" --cron "$F12/jobs.json" --once 2>&1)
echo "$CR" | grep -q 'echo round complete' || { echo "FAIL cron: $CR"; exit 1; }
[ -f "$HOME/.local/share/motiris/cron/j1.log" ] || { echo "FAIL cron log"; exit 1; }
grep -q 'rc=0' "$HOME/.local/share/motiris/cron/j1.log" || { echo "FAIL cron log rc"; cat "$HOME/.local/share/motiris/cron/j1.log"; exit 1; }
MOTIRIS_HOME="$F12/h" python3 -c '
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            c = str(msgs[-1].get("content",""))[:120]
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"SUBOK "+c}}]}
        else:
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_s1","type":"function","function":{"name":"subagent","arguments":json.dumps({"task":"sub hi"})}}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18095),H).serve_forever()
' &
M12PID=$!
MOCK_PIDS="$MOCK_PIDS $M12PID"
sleep 0.5
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18095/v1/chat/completions"}' > "$F12/h/config.json"
O5=$(MOTIRIS_HOME="$F12/h" MOTIRIS_BIN="$ROOT/motiris" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O5" | grep -q 'SUBOK' || { echo "FAIL subagent: $O5"; exit 1; }
kill $M12PID 2>/dev/null
wait $M12PID 2>/dev/null || true
rm -rf "$F12"

echo "== 13: MCP server bridge + browser shim"
F13=$(mktemp -d)
mkdir -p "$F13/h" "$F13/bin"
cat > "$F13/mcp_server.py" <<'PYEOF'
import json, sys
def reply(m): sys.stdout.write(json.dumps(m) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    try: req = json.loads(line)
    except Exception: continue
    m = req.get("method"); rid = req.get("id")
    if m == "initialize":
        reply({"jsonrpc":"2.0","id":rid,"result":{"serverInfo":{"name":"fake","version":"1"}}})
    elif m == "tools/list":
        reply({"jsonrpc":"2.0","id":rid,"result":{"tools":[{"name":"greet","description":"hi","inputSchema":{"type":"object"}}]}})
    elif m == "tools/call":
        a = req.get("params",{}).get("arguments",{}) or {}
        who = a.get("who","world")
        reply({"jsonrpc":"2.0","id":rid,"result":{"content":[{"type":"text","text":"hello, "+str(who)+"!"}]}})
PYEOF
cat > "$F13/bin/chromium" <<'SHEOF'
#!/bin/sh
echo '<html><body><div id="app">rendered by js</div></body></html>'
SHEOF
chmod +x "$F13/mcp_server.py" "$F13/bin/chromium"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18097/v1/chat/completions","mcp_servers":[{"name":"fake","cmd":"python3","args":["%s/mcp_server.py"]}]}' "$F13" > "$F13/h/config.json"
python3 - <<'PY2' &
import http.server, json, threading
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            c = str(msgs[-1].get("content",""))[:120]
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"M:"+c}}]}
        else:
            if self.server.server_address[1] == 18097:
                tc = {"name":"fake:greet","arguments":json.dumps({"who":"motiris"})}
            else:
                tc = {"name":"browser_fetch","arguments":json.dumps({"url":"http://example.com/"})}
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_z1","type":"function","function":tc}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
for port in (18097,18098):
    s = http.server.HTTPServer(("127.0.0.1",port),H)
    threading.Thread(target=s.serve_forever,daemon=True).start()
import time
time.sleep(60)
PY2
M13PID=$!
MOCK_PIDS="$MOCK_PIDS $M13PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -qE '1809[78]' && break
  sleep 0.3
done
O6=$(MOTIRIS_HOME="$F13/h" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O6" | grep -q 'hello, motiris!' || { echo "FAIL mcp: $O6"; exit 1; }
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18098/v1/chat/completions"}' > "$F13/h/config.json"
O7=$(PATH="$F13/bin:$PATH" MOTIRIS_BROWSER=chromium MOTIRIS_HOME="$F13/h" "$ROOT/motiris" -p hi --max-steps 3 -k x 2>&1)
echo "$O7" | grep -q 'rendered by js' || { echo "FAIL browser: $O7"; exit 1; }
kill $M13PID 2>/dev/null
wait $M13PID 2>/dev/null || true
rm -rf "$F13"

echo "== 14: SSE streaming aggregation (delta merge)"
F14=$(mktemp -d)
mkdir -p "$F14/h"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18100/v1/chat/completions","stream":true}' > "$F14/h/config.json"
python3 - <<'PY4' &
import http.server, threading
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length",0)))
        body = "".join(
            'data: {"choices":[{"delta":{"content":"%s"}}]}\n\n' % c
            for c in ("hel", "lo ", "world")) + "data: [DONE]\n\n"
        b = body.encode()
        self.send_response(200)
        self.send_header("Content-Type","text/event-stream")
        self.send_header("Content-Length",str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18100),H).serve_forever()
PY4
M14PID=$!
MOCK_PIDS="$MOCK_PIDS $M14PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -q 18100 && break
  sleep 0.3
done
O8=$(MOTIRIS_HOME="$F14/h" "$ROOT/motiris" -p hi --max-steps 1 --stream -k x 2>&1)
echo "$O8" | grep -q 'hello world' || { echo "FAIL stream: $O8"; exit 1; }
kill $M14PID 2>/dev/null
wait $M14PID 2>/dev/null || true
rm -rf "$F14"

echo "== 15: repl via pipe (commands + run + exit)"
O9=$(printf '/help\nhi\n/exit\n' | "$ROOT/motiris" -i --transport echo --no-plugin 2>&1)
echo "$O9" | grep -q 'echo round complete' || { echo "FAIL repl run: $O9"; exit 1; }
echo "$O9" | grep -q '/help' || { echo "FAIL repl help: $O9"; exit 1; }
echo "$O9" | grep -q 'bye' || { echo "FAIL repl exit: $O9"; exit 1; }

echo "== 16: repl shows tool calls + token stats"
F16=$(mktemp -d)
mkdir -p "$F16/h"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18101/v1/chat/completions"}' > "$F16/h/config.json"
python3 - <<'PY5' &
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = __import__("json").loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            d = ("data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                 "data: {\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":10}}\n\n"
                 "data: [DONE]\n\n")
        else:
            d = ("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_t1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"\"}}]}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\": \\\"x.txt\\\"}\"}}]}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                 "data: {\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":25}}\n\n"
                 "data: [DONE]\n\n")
        b = d.encode()
        self.send_response(200)
        self.send_header("Content-Type","text/event-stream")
        self.send_header("Content-Length",str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18101),H).serve_forever()
PY5
M16PID=$!
MOCK_PIDS="$MOCK_PIDS $M16PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -q 18101 && break
  sleep 0.3
done
O10=$(MOTIRIS_HOME="$F16/h" MOTIRIS_WORKSPACE="$F16" sh -c 'printf "hi\n/exit\n" | "$0" -i -k x' "$ROOT/motiris" 2>&1)
echo "$O10" | grep -q 'read_file' || { echo "FAIL tool line: $O10"; exit 1; }
echo "$O10" | grep -q '↑140' || { echo "FAIL tokens in: $O10"; exit 1; }
echo "$O10" | grep -q '↓35' || { echo "FAIL tokens out: $O10"; exit 1; }
kill $M16PID 2>/dev/null
wait $M16PID 2>/dev/null || true
rm -rf "$F16"

echo "== 17: plugin tool hook fires on tool calls"
F17=$(mktemp -d)
mkdir -p "$F17/h" "$F17/pd"
cp "$ROOT/examples/hello_plugin.so" "$F17/pd/"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18102/v1/chat/completions"}' > "$F17/h/config.json"
python3 - <<'PY6' &
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = __import__("json").loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            r = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"greeted"}}]}
        else:
            r = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_h1","type":"function","function":{"name":"hello","arguments":"{\"name\": \"iris\"}"}}]}}]}
        b = __import__("json").dumps(r).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18102),H).serve_forever()
PY6
M17PID=$!
MOCK_PIDS="$MOCK_PIDS $M17PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -q 18102 && break
  sleep 0.3
done
O11=$(MOTIRIS_HOME="$F17/h" MOTIRIS_WORKSPACE="$F17" "$ROOT/motiris" -p hi --max-steps 3 --plugin-dir "$F17/pd" -k x 2>&1)
echo "$O11" | grep -q 'tool invoked: hello' || { echo "FAIL plugin hook: $O11"; exit 1; }
echo "$O11" | grep -q 'greeted' || { echo "FAIL plugin result: $O11"; exit 1; }
kill $M17PID 2>/dev/null
wait $M17PID 2>/dev/null || true
rm -rf "$F17"

echo "== 18: repl banner shows model / skills / tools"
F18=$(mktemp -d)
mkdir -p "$F18/skills"
printf -- '---\nname: banner-skill\ndescription: demo\n---\nbody\n' > "$F18/skills/banner.md"
O12=$(printf '/exit\n' | MOTIRIS_MODEL=banner-model "$ROOT/motiris" -i \
      --transport echo --no-plugin --skill-dir "$F18/skills" 2>&1)
echo "$O12" | grep -q 'banner-model' || { echo "FAIL banner model: $O12"; exit 1; }
echo "$O12" | grep -q 'banner-skill' || { echo "FAIL banner skill: $O12"; exit 1; }
echo "$O12" | grep -q 'shell' || { echo "FAIL banner tools: $O12"; exit 1; }
rm -rf "$F18"

echo "== 19: repl /skills lists skills from --skill-dir"
F19=$(mktemp -d)
printf -- '---\nname: skill-nineteen\ndescription: filter me\n---\nbody\n' > "$F19/nineteen.md"
O13=$(printf '/skills\n/exit\n' | "$ROOT/motiris" -i --transport echo \
      --no-plugin --skill-dir "$F19" 2>&1)
echo "$O13" | grep -q 'skill-nineteen' || { echo "FAIL /skills: $O13"; exit 1; }
echo "$O13" | grep -q 'filter me' || { echo "FAIL /skills desc: $O13"; exit 1; }
rm -rf "$F19"

echo "== 20: skill_patch edits skill file via agent loop"
F20=$(mktemp -d)
mkdir -p "$F20/h" "$F20/skills"
printf -- '---\nname: patchme\ndescription: to patch\n---\nold body text\n' > "$F20/skills/patchme.md"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18103/v1/chat/completions"}' > "$F20/h/config.json"
python3 - <<'PY20' &
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"patched ok"}}]}
        else:
            tc = {"name":"skill_patch","arguments":json.dumps({"name":"patchme","old":"old body text","new":"new body text"})}
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_p1","type":"function","function":tc}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18103),H).serve_forever()
PY20
M20PID=$!
MOCK_PIDS="$MOCK_PIDS $M20PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -q 18103 && break
  sleep 0.3
done
O14=$(MOTIRIS_HOME="$F20/h" MOTIRIS_WORKSPACE="$F20" "$ROOT/motiris" -p hi --max-steps 3 --skill-dir "$F20/skills" -k x 2>&1)
echo "$O14" | grep -q 'patched ok' || { echo "FAIL skill_patch run: $O14"; exit 1; }
grep -q 'new body text' "$F20/skills/patchme.md" || { echo "FAIL skill_patch file"; cat "$F20/skills/patchme.md"; exit 1; }
kill $M20PID 2>/dev/null
wait $M20PID 2>/dev/null || true
rm -rf "$F20"

echo "== 21: skill_write rewrites skill frontmatter via agent loop"
F21=$(mktemp -d)
mkdir -p "$F21/h" "$F21/skills"
printf -- '---\nname: writeme\ndescription: old desc\n---\nold body\n' > "$F21/skills/writeme.md"
printf '{"transport":"libcurl","base_url":"http://127.0.0.1:18104/v1/chat/completions"}' > "$F21/h/config.json"
python3 - <<'PY21' &
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length",0))))
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            reply = {"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"written ok"}}]}
        else:
            content = "---\nname: newname\ndescription: new desc\n---\nnew body\n"
            tc = {"name":"skill_write","arguments":json.dumps({"name":"writeme","content":content})}
            reply = {"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_w2","type":"function","function":tc}]}}]}
        b = json.dumps(reply).encode()
        self.send_response(200); self.send_header("Content-Length",str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
http.server.HTTPServer(("127.0.0.1",18104),H).serve_forever()
PY21
M21PID=$!
MOCK_PIDS="$MOCK_PIDS $M21PID"
for _i in 1 2 3 4 5 6 7 8; do
  ss -ltn 2>/dev/null | grep -q 18104 && break
  sleep 0.3
done
O16=$(MOTIRIS_HOME="$F21/h" MOTIRIS_WORKSPACE="$F21" "$ROOT/motiris" -p hi --max-steps 3 --skill-dir "$F21/skills" -k x 2>&1)
echo "$O16" | grep -q 'written ok' || { echo "FAIL skill_write run: $O16"; exit 1; }
grep -q 'name: newname' "$F21/skills/writeme.md" || { echo "FAIL skill_write file"; cat "$F21/skills/writeme.md"; exit 1; }
kill $M21PID 2>/dev/null
wait $M21PID 2>/dev/null || true
rm -rf "$F21"

echo "== 22: repl /sessions lists session logs"
F22=$(mktemp -d)
mkdir -p "$F22/.local/share/motiris/sessions"
printf 'Uhello from a past session\n' > "$F22/.local/share/motiris/sessions/fake-1.log"
HOME="$F22" sh -c 'printf "/sessions\n/exit\n" | "$0" -i --transport echo --no-plugin 2>&1' "$ROOT/motiris" > "$F22/out.txt"
grep -q 'fake-1.log' "$F22/out.txt" || { echo "FAIL /sessions: $(cat "$F22/out.txt")"; exit 1; }
rm -rf "$F22"

echo "== 23: repl /resume loads U rows into context"
F23=$(mktemp -d)
printf 'Uresumed turn one\nAold reply\n' > "$F23/old.log"
O15=$(printf '/resume %s/old.log\n/exit\n' "$F23" | "$ROOT/motiris" -i \
      --transport echo --no-plugin 2>&1)
echo "$O15" | grep -q 'loaded 1 turns from' || { echo "FAIL /resume: $O15"; exit 1; }
rm -rf "$F23"

echo "smoke: all tests passed"