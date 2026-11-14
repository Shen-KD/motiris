#!/bin/sh
# Optional TTS_TOOLS=1 host adapter tests; no audio device or network needed.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd) python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

root = Path(os.environ["ROOT"])
binary = root / "motiris"


def sse_text(text):
    return ("data: " + json.dumps({"choices": [{"delta": {"content": text}}]})
            + "\n\n"
            + "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            + "data: [DONE]\n\n").encode()


class Provider(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.responses = []
        self.requests = []
        self.ready = threading.Event()

    def run(self):
        import http.server
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                outer.requests.append(request)
                if "Goal check:" in str(request["messages"][-1].get("content", "")):
                    text = "DONE"
                else:
                    text = outer.responses.pop(0)
                body = sse_text(text)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.ready.set()
        self.server.serve_forever()


provider = Provider()
provider.start()
assert provider.ready.wait(2), "local provider did not start"
endpoint = "http://127.0.0.1:%d/v1/chat/completions" % provider.port

try:
    with tempfile.TemporaryDirectory(prefix="motiris-tts-") as td:
        tmp = Path(td)
        home = tmp / "home"
        home.mkdir()
        config = tmp / "config"
        config.mkdir()
        capture = tmp / "captured.txt"
        engine_pid = tmp / "engine.pid"
        engine = tmp / "fake-tts"
        engine.write_text("""#!/usr/bin/env python3
import os
from pathlib import Path
import sys
import time

mode = os.environ.get('FAKE_TTS_MODE', 'ok')
if mode == 'timeout':
    Path(os.environ['FAKE_TTS_PID']).write_text(str(os.getpid()))
    time.sleep(60)
elif mode == 'fail':
    sys.stdin.buffer.read(1)
    raise SystemExit(23)
else:
    Path(os.environ['FAKE_TTS_CAPTURE']).write_bytes(sys.stdin.buffer.read())
""")
        engine.chmod(0o755)

        base_env = os.environ.copy()
        for key in list(base_env):
            if key.startswith("MOTIRIS_") or key.startswith("FAKE_TTS_"):
                del base_env[key]
        base_env.update({
            "HOME": str(home),
            "MOTIRIS_HOME": str(config),
            "MOTIRIS_BASE_URL": endpoint,
            "MOTIRIS_TTS_CMD": str(engine),
            "FAKE_TTS_CAPTURE": str(capture),
            "FAKE_TTS_PID": str(engine_pid),
        })

        def run_reply(text, mode="ok", extra_env=None, extra_args=None):
            capture.unlink(missing_ok=True)
            engine_pid.unlink(missing_ok=True)
            provider.responses.append(text)
            provider.requests.clear()
            env = base_env.copy()
            env["FAKE_TTS_MODE"] = mode
            if extra_env:
                env.update(extra_env)
            cmd = [str(binary), "-i", "--transport", "libcurl", "--no-plugin",
                   "--no-tools", "-k", "offline"]
            if extra_args:
                cmd.extend(extra_args)
            started = time.monotonic()
            proc = subprocess.run(cmd, input="hello\n/exit\n", text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=env, timeout=15)
            elapsed = time.monotonic() - started
            assert proc.returncode == 0, (proc.returncode, proc.stdout)
            assert text in proc.stdout, proc.stdout[-2000:]
            assert "bye" in proc.stdout, proc.stdout[-1000:]
            return proc.stdout, elapsed

        # Final assistant text reaches the fake audio sink exactly once.
        out, _ = run_reply("speech boundary 语音")
        assert capture.read_bytes() == "speech boundary 语音".encode(), capture.read_bytes()
        assert "tts:" not in out, out

        # Backend failure never removes or fails the textual response.
        out, _ = run_reply("text survives failure", mode="fail")
        assert "engine exited with status 23" in out, out
        assert "text output preserved" in out, out

        # A non-reading engine is killed at the configured monotonic deadline.
        out, elapsed = run_reply("text survives timeout", mode="timeout",
                                 extra_env={"MOTIRIS_TTS_TIMEOUT_S": "1"})
        assert "engine timed out after 1 s" in out, out
        assert elapsed < 8, elapsed
        pid = int(engine_pid.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("timed-out fake engine is still running")

        # Input is capped even when the model emits a much longer reply.
        long_text = "x" * 17000
        out, _ = run_reply(long_text)
        assert capture.read_bytes() == b"x" * 16384, len(capture.read_bytes())
        assert "spoke only the first 16384 bytes" in out, out[-1000:]

        # Goal-check's synthetic DONE is not sent to the audio sink.
        out, _ = run_reply("real answer before confirmation",
                           extra_args=["--goal-check"])
        assert capture.read_text() == "real answer before confirmation"
        assert len(provider.requests) == 2, len(provider.requests)

        # Missing executable and invalid timeout both degrade to text.
        out, _ = run_reply("missing engine fallback",
                           extra_env={"MOTIRIS_TTS_CMD": str(tmp / "missing")})
        assert "engine exited with status 127" in out, out
        out, _ = run_reply("invalid timeout fallback",
                           extra_env={"MOTIRIS_TTS_TIMEOUT_S": "0"})
        assert "must be 1..120" in out, out

        # Runtime-disabled optional build never starts an engine.
        capture.unlink(missing_ok=True)
        provider.responses.append("plain text only")
        env = base_env.copy()
        del env["MOTIRIS_TTS_CMD"]
        proc = subprocess.run(
            [str(binary), "-i", "--transport", "libcurl", "--no-plugin",
             "--no-tools", "-k", "offline"],
            input="hello\n/exit\n", text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=env, timeout=15)
        assert proc.returncode == 0 and "plain text only" in proc.stdout, proc.stdout
        assert not capture.exists(), "disabled adapter invoked fake engine"

        print("tts: 8 host-only adapter cases passed (fake engine, no audio device)")
finally:
    provider.server.shutdown()
    provider.server.server_close()
    provider.join(timeout=5)
PY
