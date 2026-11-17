#!/usr/bin/env python3
"""Offline model-driven integration tests for the persistent memory tools."""
import http.server
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parent.parent
ACTIONS = []


class Provider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = request.get("messages", [])
        if messages and messages[-1].get("role") == "tool":
            content = messages[-1]["content"]
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": content}}]}
        else:
            name, arguments = ACTIONS.pop(0)
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": [{
                    "id": "memory-call", "type": "function", "function": {
                        "name": name, "arguments": json.dumps(arguments)}}]}}]}
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            home = base / "home"
            config = base / "config"
            home.mkdir()
            config.mkdir()
            (config / "config.json").write_text(json.dumps({
                "transport": "libcurl",
                "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                            server.server_address[1],
            }))
            memory = home / ".local/share/motiris/memory.json"
            time_binary = ("/usr/bin/time"
                           if os.path.isfile("/usr/bin/time") and
                           os.access("/usr/bin/time", os.X_OK) else None)

            def invoke(name, arguments, file_limit=False, measure=False,
                       timeout=30):
                ACTIONS.append((name, arguments))
                env = os.environ.copy()
                for key in list(env):
                    if key.startswith("MOTIRIS_"):
                        env.pop(key)
                env.update({"HOME": str(home), "MOTIRIS_HOME": str(config)})
                command = [str(ROOT / "motiris"), "-p", "memory", "-k", "x",
                           "--max-steps", "3", "--no-plugin"]
                timing = base / "timing"
                if measure and time_binary:
                    command = [time_binary, "-f", "%M", "-o", str(timing)] + command

                def limits():
                    if file_limit:
                        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                        resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

                proc = subprocess.run(
                    command, env=env, text=True, capture_output=True,
                    timeout=timeout, preexec_fn=limits if file_limit else None)
                assert proc.returncode == 0, (proc.stdout, proc.stderr)
                lines = proc.stdout.strip().splitlines()
                assert lines, (proc.stdout, proc.stderr)
                result = json.loads(lines[-1])
                if measure and time_binary:
                    print("memory: maximum-value integration peak RSS = %s KiB" %
                          timing.read_text().strip())
                elif measure:
                    print("memory: peak RSS skipped (/usr/bin/time unavailable)")
                return result

            assert invoke("memory_get", {}) == {"keys": {}}
            assert invoke("memory_set", {"key": "planet", "value": "mars"}) == {
                "ok": "stored"}
            assert memory.stat().st_mode & 0o777 == 0o600
            assert invoke("memory_get", {"key": "planet"}) == {"value": "mars"}
            for index in range(32):
                value = "earth" if index % 2 else "mars"
                assert invoke("memory_set", {"key": "planet", "value": value}) == {
                    "ok": "stored"}
            assert invoke("memory_get", {"key": "planet"}) == {"value": "earth"}

            for invalid in (b"", b'{"important":"keep",}', b"[]",
                            b'{"keep":"yes"} trailing'):
                memory.write_bytes(invalid)
                assert "error" in invoke("memory_get", {})
                assert "error" in invoke("memory_set", {"key": "new", "value": "v"})
                assert memory.read_bytes() == invalid

            # A FIFO must be rejected by fstat, not hang in open while waiting
            # for a writer. Keep the subprocess timeout as a regression guard.
            memory.unlink()
            os.mkfifo(memory)
            assert "error" in invoke("memory_get", {}, timeout=10)
            assert "error" in invoke(
                "memory_set", {"key": "new", "value": "v"}, timeout=10)
            assert memory.is_fifo()
            memory.unlink()

            original = b'{"important":"keep"}'
            memory.write_bytes(original)
            result = invoke("memory_set", {"key": "new", "value": "v"},
                            file_limit=True)
            assert "error" in result, result
            assert memory.read_bytes() == original
            assert not list(memory.parent.glob("memory.json.tmp.*"))

            maximum = "v" * (256 * 1024)
            assert invoke("memory_set", {"key": "maximum", "value": maximum},
                          measure=True) == {"ok": "stored"}
            assert invoke("memory_get", {"key": "maximum"}) == {"value": maximum}
            assert memory.stat().st_size <= 1024 * 1024
            print("memory: round-trip, corruption/nonregular preservation, write failure, RAM and bounds passed")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
