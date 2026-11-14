#!/bin/sh
# Optional VISION_TOOLS=1 integration tests. All providers and images are local.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd) python3 - <<'PY'
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

root = Path(os.environ["ROOT"])
binary = root / "motiris"
vision_requests = []
tool_results = []
initial_requests = []


def response(handler, payload, status=200):
    body = json.dumps(payload).encode()
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def reply(content):
    return {"choices": [{"finish_reason": "stop", "message": {
        "role": "assistant", "content": content}}]}


class Provider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        messages = request["messages"]

        # Final outer-agent round: retain the tool's structured JSON result.
        if any(m.get("role") == "tool" for m in messages):
            tool = next(m for m in messages if m.get("role") == "tool")
            tool_results.append(json.loads(tool["content"]))
            response(self, reply("outer completed"))
            return

        # The optional tool sends one user with text + image_url content blocks.
        if isinstance(messages[0].get("content"), list):
            vision_requests.append((request, self.headers))
            question = messages[0]["content"][0]["text"]
            if question == "provider-error":
                response(self, {"error": {"message": "provider down"}}, 500)
            elif question == "oversized-response":
                response(self, reply("x" * 70000))
            elif question == "long-description":
                response(self, reply("y" * 17000))
            else:
                response(self, reply("small image description"))
            return

        # First outer-agent round: call the tool using the JSON prompt as args.
        initial_requests.append(request)
        args = json.loads(messages[-1]["content"])
        response(self, {"choices": [{"finish_reason": "tool_calls", "message": {
            "role": "assistant",
            "tool_calls": [{"id": "call_vision", "type": "function",
                            "function": {"name": "vision_analyze",
                                         "arguments": json.dumps(args)}}]}}]})

    def log_message(self, *args):
        pass


provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
port = provider.server_address[1]
thread = threading.Thread(target=provider.serve_forever, daemon=True)
thread.start()
endpoint = "http://127.0.0.1:%d/v1/chat/completions" % port

try:
    with tempfile.TemporaryDirectory(prefix="motiris-vision-") as td:
        tmp = Path(td)
        workspace = tmp / "workspace"
        workspace.mkdir()
        home = tmp / "home"
        home.mkdir()
        config = tmp / "config"
        config.mkdir()
        (workspace / "small.bin").write_bytes(bytes([0, 1, 2, 253, 254, 255]))
        (workspace / "five.bin").write_bytes(b"12345")
        (tmp / "outside.bin").write_bytes(b"outside")

        base_env = os.environ.copy()
        for key in list(base_env):
            if key.startswith("MOTIRIS_"):
                del base_env[key]
        base_env.update({
            "HOME": str(home),
            "MOTIRIS_HOME": str(config),
            "MOTIRIS_WORKSPACE": str(workspace),
            "MOTIRIS_BASE_URL": endpoint,
            "MOTIRIS_MODEL": "outer-test",
            "MOTIRIS_VISION_BASE_URL": endpoint,
            "MOTIRIS_VISION_MODEL": "vision-test",
            "MOTIRIS_VISION_API_KEY": "offline-test-key",
        })

        def run_case(args, max_bytes=None, vision_url=None, rss_file=None):
            env = base_env.copy()
            if max_bytes is not None:
                env["MOTIRIS_VISION_MAX_BYTES"] = str(max_bytes)
            if vision_url is not None:
                env["MOTIRIS_VISION_BASE_URL"] = vision_url
            cmd = []
            if rss_file is not None:
                cmd = ["/usr/bin/time", "-f", "%M", "-o", str(rss_file)]
            cmd += [str(binary), "--transport", "libcurl", "--no-plugin",
                    "--max-steps", "3", "-p", json.dumps(args)]
            proc = subprocess.run(cmd, cwd=workspace, env=env, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=20)
            assert proc.returncode == 0, (proc.returncode, proc.stdout, proc.stderr)
            assert proc.stdout.strip() == "outer completed", proc.stdout
            assert tool_results, "outer model did not receive a tool result"
            return tool_results[-1]

        # Happy path: exact bytes/MIME, content-block shape and provider override.
        result = run_case({"path": "small.bin", "mime": "image/png",
                           "question": "what is shown?"})
        assert result == {"description": "small image description",
                          "truncated": False, "input_bytes": 6}, result
        request, headers = vision_requests[-1]
        assert request["model"] == "vision-test"
        assert "tools" not in request
        assert headers["Authorization"] == "Bearer offline-test-key"
        blocks = request["messages"][0]["content"]
        assert [b["type"] for b in blocks] == ["text", "image_url"]
        assert blocks[0]["text"] == "what is shown?"
        prefix = "data:image/png;base64,"
        assert blocks[1]["image_url"]["url"].startswith(prefix)
        encoded = blocks[1]["image_url"]["url"][len(prefix):]
        assert base64.b64decode(encoded) == bytes([0, 1, 2, 253, 254, 255])
        assert any("vision_analyze" in json.dumps(r.get("tools", []))
                   for r in initial_requests)

        # Validation failures never send an image to the provider.
        before = len(vision_requests)
        result = run_case({"path": "small.bin", "mime": "text/plain"})
        assert "unsupported mime" in result["error"]
        assert len(vision_requests) == before
        result = run_case({"path": "five.bin", "mime": "image/png"}, max_bytes=4)
        assert "4-byte limit" in result["error"]
        assert len(vision_requests) == before
        result = run_case({"path": "../outside.bin", "mime": "image/png"})
        assert "outside workspace" in result["error"]
        assert len(vision_requests) == before
        result = run_case({"path": "small.bin", "mime": "image/png"},
                          max_bytes=1048577)
        assert "1..1048576" in result["error"]
        assert len(vision_requests) == before

        # Provider/API failures and disconnected vision fallback stay bounded.
        result = run_case({"path": "small.bin", "mime": "image/png",
                           "question": "provider-error"})
        assert "HTTP 500" in result["error"] and "provider down" in result["error"]
        result = run_case({"path": "small.bin", "mime": "image/png"},
                          vision_url="http://127.0.0.1:1/v1/chat/completions")
        assert "vision request failed" in result["error"]
        result = run_case({"path": "small.bin", "mime": "image/png",
                           "question": "oversized-response"})
        assert "exceeds 64KiB" in result["error"]
        result = run_case({"path": "small.bin", "mime": "image/png",
                           "question": "long-description"})
        assert len(result["description"]) == 16384
        assert result["truncated"] is True

        # Hard maximum: exact 1 MiB round-trip; measure host peak RSS if possible.
        maximum = bytes(range(256)) * 4096
        (workspace / "maximum.bin").write_bytes(maximum)
        rss = tmp / "rss" if shutil.which("/usr/bin/time") else None
        result = run_case({"path": "maximum.bin", "mime": "image/jpeg",
                           "question": "maximum"}, max_bytes=1048576, rss_file=rss)
        assert result["input_bytes"] == 1048576
        request, _ = vision_requests[-1]
        url = request["messages"][0]["content"][1]["image_url"]["url"]
        decoded = base64.b64decode(url.split(",", 1)[1])
        assert len(decoded) == 1048576
        assert hashlib.sha256(decoded).digest() == hashlib.sha256(maximum).digest()
        if rss is not None:
            print("vision max-input peak RSS: %s KiB" % rss.read_text().strip())

        print("vision: 10 integration cases passed (local mock only)")
finally:
    provider.shutdown()
    provider.server_close()
    thread.join(timeout=5)
PY
