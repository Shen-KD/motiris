#!/usr/bin/env python3
"""Stream-enabled REPL rendering and fail-after-delta regressions."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"


def sse(text):
    event = {"choices": [{"delta": {"content": text}}]}
    return b"data: " + json.dumps(event, separators=(",", ":")).encode() + b"\n\n"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.hits = {}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.hits[self.path] = self.server.hits.get(self.path, 0) + 1
        if self.path == "/goal":
            confirmation = any("Goal check:" in str(m.get("content", ""))
                               for m in request.get("messages", []))
            body = sse("DONE" if confirmation else "real answer") + b"data: [DONE]\n\n"
            truncated = False
        elif self.path == "/fail":
            body = sse("partial-failed-answer")
            truncated = True
        elif self.path == "/fallback":
            body = sse("fallback-answer") + b"data: [DONE]\n\n"
            truncated = False
        else:
            body = json.dumps({"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": "spawned once"}}]},
                separators=(",", ":")).encode()
            truncated = False
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream"
                             if self.path != "/spawn" else "application/json")
            self.send_header("Content-Length", str(len(body) + (64 if truncated else 0)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def log_message(self, *_args):
        pass


def invoke(root, env, args, stdin="question\n/exit\n"):
    return subprocess.run(
        [str(BINARY), "-i", "--no-tools", "--no-plugin"] + args,
        cwd=str(ROOT), env=env, input=stdin, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=12)


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-render-") as tmp:
            root = Path(tmp)
            (root / "home").mkdir()
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("MOTIRIS_")}
            env.update({"HOME": str(root / "home"), "MOTIRIS_HOME": str(root),
                        "NO_PROXY": "127.0.0.1,localhost",
                        "no_proxy": "127.0.0.1,localhost"})
            base = "http://127.0.0.1:%d" % provider.server_port

            (root / "config.json").write_text(json.dumps({
                "transport": "libcurl", "base_url": base + "/goal", "stream": True}))
            goal = invoke(root, env, ["--goal-check", "--max-steps", "3"])
            if goal.returncode or goal.stdout.count("real answer") != 1 or "DONE" in goal.stdout:
                raise AssertionError("streamed goal-check rendering: %s" % goal.stdout)

            (root / "config.json").write_text(json.dumps({
                "transport": "libcurl", "stream": True,
                "providers": [
                    {"name": "failed", "model": "failed", "base_url": base + "/fail"},
                    {"name": "fallback", "model": "fallback", "base_url": base + "/fallback"},
                ]}))
            failed = invoke(root, env, ["--max-steps", "1"])
            if ("partial-failed-answer" not in failed.stdout or
                    "fallback-answer" in failed.stdout or
                    "transport error" not in failed.stdout or
                    provider.hits.get("/fallback", 0)):
                raise AssertionError("fail-after-delta mixed providers: %s" % failed.stdout)

            echo = invoke(root, env, ["--transport", "echo", "--stream",
                                      "--max-steps", "2"])
            if echo.returncode or echo.stdout.count("echo round complete: [ok]") != 1:
                raise AssertionError("echo fallback rendering: %s" % echo.stdout)

            (root / "config.json").write_text(json.dumps({
                "transport": "curl", "base_url": base + "/spawn", "stream": True}))
            spawned = invoke(root, env, ["--max-steps", "1"])
            if spawned.returncode or spawned.stdout.count("spawned once") != 1:
                raise AssertionError("spawned fallback rendering: %s" % spawned.stdout)
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("render: goal-check, fail-after-delta, echo and spawned REPL checks passed")


if __name__ == "__main__":
    main()
