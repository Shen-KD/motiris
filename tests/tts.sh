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


def sse_goal_tool():
    event = {"choices": [{"delta": {"tool_calls": [{
        "index": 0, "id": "goal-tool", "type": "function",
        "function": {"name": "shell", "arguments": "{}"}
    }]}}]}
    return ("data: " + json.dumps(event) + "\n\n"
            + "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
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
                messages = request["messages"]
                prompt = str(messages[-1].get("content", ""))
                goal_prompt = (prompt.startswith("Goal check: <") and
                               "If the goal is fully achieved" in prompt)
                goal_tools = any(m.get("role") == "user" and
                                 m.get("content") == "goal-tools" for m in messages)
                after_goal_tool = (messages[-1].get("role") == "tool" and
                                   goal_tools and any(
                    str(m.get("content", "")).startswith("Goal check: <") and
                    "If the goal is fully achieved" in str(m.get("content", ""))
                    for m in messages[:-1]))
                if goal_prompt and goal_tools:
                    body = sse_goal_tool()
                else:
                    if goal_prompt or after_goal_tool:
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
        invocations = tmp / "invocations.log"
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
    data = sys.stdin.buffer.read()
    if os.environ.get('FAKE_TTS_COUNT'):
        with open(os.environ['FAKE_TTS_COUNT'], 'a') as stream:
            stream.write('invoked\\n')
    Path(os.environ['FAKE_TTS_CAPTURE']).write_bytes(data)
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
            "FAKE_TTS_COUNT": str(invocations),
            "FAKE_TTS_PID": str(engine_pid),
        })

        def run_reply(text, mode="ok", extra_env=None, extra_args=None,
                      prompt="hello"):
            capture.unlink(missing_ok=True)
            invocations.unlink(missing_ok=True)
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
            proc = subprocess.run(cmd, input=prompt + "\n/exit\n", text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=env, timeout=15)
            elapsed = time.monotonic() - started
            assert proc.returncode == 0, (proc.returncode, proc.stdout)
            assert text in proc.stdout, proc.stdout[-2000:]
            assert "bye" in proc.stdout, proc.stdout[-1000:]
            return proc.stdout, elapsed

        # Final assistant text reaches the fake audio sink exactly once.
        out, _ = run_reply("speech boundary 语音")
        assert capture.exists(), out
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
        assert capture.exists(), out
        assert capture.read_bytes() == b"x" * 16384, len(capture.read_bytes())
        assert "spoke only the first 16384 bytes" in out, out[-1000:]

        # Goal-check's synthetic DONE is not sent to the audio sink.
        out, _ = run_reply("real answer before confirmation",
                           extra_args=["--goal-check"])
        assert capture.exists(), out
        assert capture.read_text() == "real answer before confirmation"
        assert len(provider.requests) == 2, len(provider.requests)

        # Empty terminal messages end bounded and never reuse older speech.
        out, _ = run_reply("")
        assert not capture.exists(), "empty reply invoked fake engine"
        assert len(provider.requests) == 1, len(provider.requests)
        out, _ = run_reply("", extra_args=["--goal-check"])
        assert not capture.exists(), "empty goal-check reply invoked fake engine"
        assert len(provider.requests) == 2, len(provider.requests)

        # A real user can start with goal-like text. An empty second turn must
        # not repeat the first turn's speech or mistake its prompt for synthetic.
        capture.unlink(missing_ok=True)
        invocations.unlink(missing_ok=True)
        provider.responses.extend(["first speech only", ""])
        provider.requests.clear()
        proc = subprocess.run(
            [str(binary), "-i", "--transport", "libcurl", "--no-plugin",
             "--no-tools", "--goal-check", "-k", "offline"],
            input="hello\nGoal check: <user-controlled>\n/exit\n", text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=base_env, timeout=15)
        assert proc.returncode == 0, proc.stdout
        assert capture.exists(), proc.stdout
        assert capture.read_text() == "first speech only"
        assert len(invocations.read_text().splitlines()) == 1, proc.stdout
        assert len(provider.requests) == 4, len(provider.requests)

        # Confirmation may call tools before DONE; neither the tool round nor
        # the synthetic acknowledgement can replace the real pre-goal answer.
        out, _ = run_reply("real before goal tools", prompt="goal-tools",
                           extra_args=["--goal-check"])
        assert capture.read_text() == "real before goal tools"
        assert len(invocations.read_text().splitlines()) == 1
        assert len(provider.requests) == 3, len(provider.requests)

        # DONE is real content when it answers an external goal-like prompt;
        # only the later synthetic acknowledgement is suppressed.
        out, _ = run_reply("DONE", prompt="Goal check: <real-DONE>",
                           extra_args=["--goal-check"])
        assert capture.read_text() == "DONE"
        assert len(invocations.read_text().splitlines()) == 1
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

        print("tts: 13 host-only adapter cases passed (fake engine, no audio device)")
finally:
    provider.server.shutdown()
    provider.server.server_close()
    provider.join(timeout=5)
PY
