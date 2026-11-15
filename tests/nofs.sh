#!/bin/sh
# Verify the full host CLI without optional POSIX file/shell backends.
set -e
cd "$(dirname "$0")/.."

test ! -e src/tools/filetools.o
test -f src/tools/filetools_nofs.o
ROOT=$(pwd) python3 - <<'PY'
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

root = Path(os.environ["ROOT"])
captures = []
class Provider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        captures.append(req)
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            result = json.loads(msgs[-1]["content"])
            assert "file tools unavailable" in result["error"], result
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": msgs[-1]["content"]}}]}
        else:
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": [{
                    "id": "nofs", "type": "function", "function": {
                        "name": "read_file",
                        "arguments": json.dumps({"path": "secret"})}}]}}]}
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp)
        (home / "config.json").write_text(json.dumps({
            "transport": "libcurl",
            "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                        server.server_address[1],
        }))
        env = os.environ.copy()
        env["MOTIRIS_HOME"] = str(home)
        env.pop("MOTIRIS_API_KEY", None)
        proc = subprocess.run(
            [str(root / "motiris"), "-p", "read", "-k", "x", "--max-steps", "3"],
            env=env, text=True, capture_output=True, timeout=10)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        assert "file tools unavailable" in proc.stdout, proc.stdout
        assert len(captures) == 2, len(captures)
        assert "file tools unavailable" in json.dumps(captures[-1])
        tool_names = [tool["function"]["name"]
                      for tool in captures[0].get("tools", [])]
        assert "shell" not in tool_names, tool_names
        assert "time" in tool_names, tool_names
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
print("nofs: file stub + shell backend cut passed")
PY
