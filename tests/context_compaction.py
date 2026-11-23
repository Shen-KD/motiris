#!/usr/bin/env python3
"""Optional deterministic context-compaction plugin integration tests."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"


class Provider(http.server.BaseHTTPRequestHandler):
    requests = []

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        type(self).requests.append(request)
        reply = "reply %d" % len(type(self).requests)
        body = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": reply}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def run_repl(config_home, plugin_dir, port):
    config_home.mkdir(parents=True, exist_ok=True)
    (config_home / "config.json").write_text(json.dumps({
        "transport": "libcurl",
        "base_url": "http://127.0.0.1:%d/v1/chat" % port,
        "context_compaction": True,
        "compaction_threshold": 4,
        "compaction_keep": 2,
        "compaction_max_chars": 512,
    }))
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update(HOME=str(config_home), MOTIRIS_HOME=str(config_home),
               MOTIRIS_PLUGIN_DIR=str(plugin_dir))
    return subprocess.run(
        [str(BINARY), "-i"], input="one 中文\ntwo\nthree\n/exit\n",
        text=True, capture_output=True, timeout=30, env=env)


def main():
    Provider.requests = []
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-compaction-") as temp:
            root = Path(temp)
            result = run_repl(root / "enabled", ROOT / "plugins",
                              provider.server_address[1])
            assert result.returncode == 0, result.stderr
            assert len(Provider.requests) == 3, result.stderr
            third = Provider.requests[2]["messages"]
            assert len(third) == 5, third
            assert third[0]["role"] == "user", third
            assert "Context compaction: 2 older messages" in third[0]["content"], third
            assert "one 中文" in third[0]["content"], third
            assert third[-1]["content"] == "three", third
            assert third[-2]["role"] == "assistant", third
            # Summary text stays well below the request/history cap and is canonical.
            assert len(third[0]["content"]) <= 512, third
            session = root / "enabled/session/session.json"
            assert session.exists(), list(session.parent.iterdir())

            Provider.requests.clear()
            missing = run_repl(root / "missing", root / "empty-plugins",
                               provider.server_address[1])
            assert len(Provider.requests) == 2, Provider.requests
            assert "plugin is unavailable" in missing.stderr, missing.stderr
            assert "reply 3" not in missing.stdout, missing.stdout
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("context compaction: bounded summary, current turn and fail-safe missing plugin passed")


if __name__ == "__main__":
    main()
