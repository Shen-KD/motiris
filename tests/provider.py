#!/usr/bin/env python3
"""Provider-list routing regressions, including the singleton case."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append({
            "json": json.loads(body),
            "authorization": self.headers.get("Authorization"),
        })
        response = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": "configured provider ok"}}]},
            separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *_args):
        pass


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-provider-") as tmp:
            root = Path(tmp)
            home = root / "home"
            home.mkdir()
            url = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port
            for count in (1, 2, 8):
                before = len(provider.requests)
                providers = []
                env = {key: value for key, value in os.environ.items()
                       if not key.startswith("MOTIRIS_")}
                env.update({
                    "HOME": str(home), "MOTIRIS_HOME": str(root),
                    "MOTIRIS_API_KEY": "default-key",
                    "NO_PROXY": "127.0.0.1,localhost",
                    "no_proxy": "127.0.0.1,localhost",
                })
                for index in range(count):
                    selected = index == count - 1
                    key_name = "PROVIDER_KEY_%d_%d" % (count, index)
                    env[key_name] = "key-%d-%d" % (count, index)
                    providers.append({
                        "name": "p%d" % index,
                        "model": "model-%d-%d" % (count, index),
                        "base_url": url if selected else
                                  "http://127.0.0.1:1/v1/chat/completions",
                        "api_key_env": key_name,
                    })
                (root / "config.json").write_text(json.dumps({
                    "transport": "libcurl", "model": "default-model",
                    "base_url": "http://127.0.0.1:1/v1/chat/completions",
                    "providers": providers,
                }))
                proc = subprocess.run(
                    [str(BINARY), "-p", "route", "--no-tools", "--no-plugin",
                     "--max-steps", "1"], cwd=str(ROOT), env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    timeout=10)
                if proc.returncode or proc.stdout != "configured provider ok\n":
                    raise AssertionError("provider count %d failed: rc=%d\n%s\n%s" %
                                         (count, proc.returncode, proc.stdout,
                                          proc.stderr))
                if len(provider.requests) != before + 1:
                    raise AssertionError("provider count %d reached default/unexpected endpoint" %
                                         count)
                request = provider.requests[-1]
                expected_model = "model-%d-%d" % (count, count - 1)
                expected_key = "Bearer key-%d-%d" % (count, count - 1)
                if request["json"].get("model") != expected_model:
                    raise AssertionError("wrong model for count %d: %r" %
                                         (count, request["json"].get("model")))
                if request["authorization"] != expected_key:
                    raise AssertionError("wrong key for count %d: %r" %
                                         (count, request["authorization"]))
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("provider: 1/2/8-entry routing, model and key selection passed")


if __name__ == "__main__":
    main()
