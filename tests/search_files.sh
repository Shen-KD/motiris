#!/bin/sh
# Security/resource regressions for search_files (issue #35).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd) python3 - <<'PY'
import http.server
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import threading
import time

root = Path(os.environ["ROOT"])
binary = root / "motiris"
captures = []
next_args = {}

class Provider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        captures.append(req)
        msgs = req.get("messages", [])
        if msgs and msgs[-1].get("role") == "tool":
            # Preserve the complete tool result so assertions cover exactly what
            # would be sent back to the provider and exposed to the model.
            content = msgs[-1]["content"]
            json.loads(content)
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": content}}]}
        else:
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": [{
                    "id": "search", "type": "function", "function": {
                        "name": "search_files", "arguments": json.dumps(next_args)}}]}}]}
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

def run_search(workspace, home, path, pattern):
    global next_args
    next_args = {"path": path, "pattern": pattern}
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env["MOTIRIS_HOME"] = str(home)
    env["MOTIRIS_WORKSPACE"] = str(workspace)
    env.pop("MOTIRIS_API_KEY", None)
    proc = subprocess.run(
        [str(binary), "-p", "search", "-k", "x", "--max-steps", "3"],
        env=env, text=True, capture_output=True, timeout=10)
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    return json.loads(lines[-1])

try:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        ws = base / "ws"
        outside = base / "outside"
        normal = ws / "normal"
        normal.mkdir(parents=True)
        outside.mkdir()
        secret = "OUTSIDE-SECRET-7f3c91"
        (outside / "secret.txt").write_text(secret + "\n")
        (normal / "inside.txt").write_text("INSIDE-OK\n")
        os.symlink(outside, ws / "linked-dir")
        os.symlink(outside / "secret.txt", ws / "linked-file")
        os.symlink(".", ws / "loop")
        os.symlink("normal", ws / "internal-link")

        home = base / "home"
        home.mkdir()
        (home / "config.json").write_text(json.dumps({
            "transport": "libcurl",
            "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                        server.server_address[1],
            "workspace_guard": True,
        }))

        result = run_search(ws, home, ".", "INSIDE-OK|OUTSIDE-SECRET")
        assert result["count"] == 1, result
        assert result["matches"][0]["text"] == "INSIDE-OK", result
        assert result["skipped_symlinks"] >= 4, result
        assert secret not in json.dumps(result), result
        assert str(outside) not in json.dumps(result), result

        # An explicitly supplied escaping link is rejected by canonical resolve.
        result = run_search(ws, home, "linked-file", "OUTSIDE-SECRET")
        assert "outside workspace" in result["error"], result
        assert secret not in json.dumps(result), result

        # A link loop is skipped, not recursively followed.
        result = run_search(ws, home, "loop", "INSIDE-OK|OUTSIDE-SECRET")
        assert "outside workspace" not in json.dumps(result), result
        assert result["count"] == 1 and result["skipped_symlinks"] >= 1, result

        # More than 32 simultaneously open directory levels must fail closed.
        deep = ws / "deep"
        cursor = deep
        for i in range(40):
            cursor = cursor / ("d%02d" % i)
        cursor.mkdir(parents=True)
        (cursor / "bottom.txt").write_text("DEEP-MARKER\n")
        result = run_search(ws, home, "deep", "DEEP-MARKER")
        assert result["truncated"] and "depth limit" in result["error"], result

        # Node exhaustion is also explicit and bounded.
        nodes = ws / "nodes"
        nodes.mkdir()
        for i in range(4105):
            (nodes / ("f%04d" % i)).write_text("")
        result = run_search(ws, home, "nodes", "NEVER")
        assert result["nodes"] <= 4096, result
        assert result["truncated"] and "node limit" in result["error"], result

        # Approach PATH_MAX and the 200-result/raw-output bounds together.
        results_dir = ws / "results"
        cursor = results_dir
        for _ in range(15):
            cursor = cursor / ("p" * 240)
        cursor.mkdir(parents=True)
        for i in range(200):
            (cursor / ("match%03d" % i)).write_text("x" * 4000 + "\n")
        result = run_search(ws, home, "results", "xxxx")
        assert result["count"] == 200, result
        assert result["truncated"] and "result limit" in result["error"], result
        assert len(result["matches"][0]["file"]) > 3000, result
        assert result["overlong_lines"] == 0, result

        # Churn a link while searching to exercise fstatat/openat replacement.
        race = ws / "race"
        race.mkdir()
        for i in range(150):
            (race / ("f%03d" % i)).write_text("RACE-INSIDE\n")
        stop = False
        def churn():
            link = race / "changing"
            tmp_link = race / "changing.tmp"
            while not stop:
                for target in (outside, normal):
                    try:
                        if os.path.lexists(tmp_link):
                            tmp_link.unlink()
                        os.symlink(target, tmp_link)
                        os.replace(tmp_link, link)
                    except FileNotFoundError:
                        pass
        changer = threading.Thread(target=churn, daemon=True)
        changer.start()
        try:
            for _ in range(4):
                result = run_search(ws, home, "race", "RACE-INSIDE|OUTSIDE-SECRET")
                assert secret not in json.dumps(result), result
                assert str(outside) not in json.dumps(result), result
        finally:
            stop = True
            changer.join(timeout=5)

        # No request, including the post-tool provider round, may contain the
        # external marker or path. This guards against accidental exfiltration.
        for request in captures:
            encoded = json.dumps(request)
            assert secret not in encoded
            assert str(outside) not in encoded
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
peak_rss_kib = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print("search_files: symlink/TOCTOU/depth/node/path/result/provider tests passed")
print("search_files: max child peak RSS %d KiB (Linux ru_maxrss)" % peak_rss_kib)
PY
