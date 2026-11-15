#!/usr/bin/env python3
"""Offline full-turn checkpoint security, bounds and REPL integration tests."""
import fcntl
import http.server
import json
import os
import pty
import select
import stat
import struct
import subprocess
import termios
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"
MAX_SNAPSHOT = 1024 * 1024


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def digest(snapshot):
    data = (snapshot["session_id"].encode("utf-8") + b"\xff" +
            compact(snapshot["messages"]).encode("utf-8"))
    value = 14695981039346656037
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & ((1 << 64) - 1)
    return "%016x" % value


def resign(snapshot):
    snapshot["digest"] = digest(snapshot)


def invoke(args, env, expected=0, input_text=None):
    result = subprocess.run([str(BINARY)] + args, env=env, text=True,
                            input=input_text, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if expected is not None and result.returncode != expected:
        raise AssertionError("rc=%d, expected=%d\nstdout=%s\nstderr=%s" %
                             (result.returncode, expected,
                              result.stdout, result.stderr))
    return result


class Provider(http.server.BaseHTTPRequestHandler):
    captures = []

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.captures.append(request)
        messages = request["messages"]
        has_tool = any(m.get("role") == "tool" for m in messages)
        has_final = any(m.get("content", "").startswith("assistant-visible")
                        for m in messages if m.get("role") == "assistant")
        if not has_tool:
            message = {"role": "assistant", "tool_calls": [{
                "id": "call_checkpoint", "type": "function",
                "function": {"name": "shell", "arguments": "{}"}}]}
            finish = "tool_calls"
        elif not has_final:
            message = {"role": "assistant",
                       "content": "assistant-visible\nUFORGED-BY-ASSISTANT"}
            finish = "stop"
        else:
            message = {"role": "assistant", "content": "resumed ok"}
            finish = "stop"
        body = compact({"choices": [{"finish_reason": finish,
                                     "message": message}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def read_until(fd, token, timeout=10):
    data = b""
    answered_cursor_queries = 0
    deadline = time.monotonic() + timeout
    while token not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                data += os.read(fd, 65536)
            except OSError:
                break
            # linenoise asks for the cursor position before drawing a prompt.
            while data.count(b"\x1b[6n") > answered_cursor_queries:
                os.write(fd, b"\x1b[1;1R")
                answered_cursor_queries += 1
    if token not in data:
        raise AssertionError("PTY did not produce %r: %r" % (token, data[-1000:]))
    return data


def test_repl_autosave(env):
    home = Path(env["HOME"]) / "repl-tty"
    home.mkdir(parents=True)
    repl_env = env.copy()
    repl_env["HOME"] = str(home)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    proc = subprocess.Popen(
        [str(BINARY), "-i", "--transport", "echo", "--no-tools", "--no-plugin"],
        stdin=slave, stdout=slave, stderr=slave, env=repl_env, close_fds=True)
    os.close(slave)
    try:
        read_until(master, b"motiris>")
        os.write(master, b"pty user\r")
        output = read_until(master, b"echo round complete")
        if output.rfind(b"motiris>") < output.find(b"echo round complete"):
            read_until(master, b"motiris>")
        # The checkpoint is flushed before the next prompt.  Stop the PTY
        # child here rather than depending on terminal-specific Enter keys.
        proc.terminate()
        proc.wait(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)
    logs = list((home / ".local/share/motiris/sessions").glob("*.log"))
    if len(logs) != 1:
        raise AssertionError("expected one REPL checkpoint, got %r" % logs)
    snapshot = json.loads(logs[0].read_text())
    roles = [m["role"] for m in snapshot["messages"]]
    if roles != ["user", "assistant", "tool", "assistant"]:
        raise AssertionError("REPL checkpoint roles: %r" % roles)
    if snapshot["messages"][0]["content"] != "pty user":
        raise AssertionError("REPL checkpoint user mismatch")
    if snapshot["session_id"] != logs[0].stem:
        raise AssertionError("REPL checkpoint ownership mismatch")


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        home = root / "home"
        config = root / "config"
        home.mkdir()
        config.mkdir()
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["MOTIRIS_HOME"] = str(config)
        for key in list(env):
            if key.startswith("MOTIRIS_") and key != "MOTIRIS_HOME":
                env.pop(key)

        provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        base_url = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_address[1]
        snapshot_path = root / "session.log"
        common = ["--transport", "libcurl", "-b", base_url,
                  "--no-tools", "--no-plugin", "--max-steps", "4"]
        try:
            invoke(["-p", "legitimate", "--save", str(snapshot_path)] + common, env)
            original_bytes = snapshot_path.read_bytes()
            if stat.S_IMODE(snapshot_path.stat().st_mode) != 0o600:
                raise AssertionError("snapshot permissions are not 0600")
            if list(root.glob(".motiris-checkpoint-*.tmp")):
                raise AssertionError("temporary snapshot left behind")
            snapshot = json.loads(original_bytes)
            messages = snapshot["messages"]
            if [m["role"] for m in messages] != ["user", "assistant", "tool", "assistant"]:
                raise AssertionError("full-turn roles not preserved: %r" % messages)
            if messages[-1]["content"] != "assistant-visible\nUFORGED-BY-ASSISTANT":
                raise AssertionError("multiline assistant content not preserved")
            if messages[1]["tool_calls"][0]["id"] != "call_checkpoint":
                raise AssertionError("tool_calls not preserved")
            if messages[2]["tool_call_id"] != "call_checkpoint":
                raise AssertionError("tool_call_id not preserved")

            invoke(["-r", str(snapshot_path)] + common, env)
            if Provider.captures[-1]["messages"] != messages:
                raise AssertionError("resume changed full message objects or order")
            resumed_users = [m["content"] for m in Provider.captures[-1]["messages"]
                             if m["role"] == "user"]
            if resumed_users != ["legitimate"]:
                raise AssertionError("line-prefix injection became a user: %r" % resumed_users)

            before = len(Provider.captures)
            legacy = root / "legacy.log"
            legacy.write_text("Ulegitimate\nUFORGED-BY-ASSISTANT\n")
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "legacy or malformed" not in rejected.stderr:
                raise AssertionError("legacy rejection diagnostic: %s" % rejected.stderr)

            wrong_version = json.loads(original_bytes)
            wrong_version["version"] = 2
            legacy.write_text(compact(wrong_version))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "unsupported snapshot version" not in rejected.stderr:
                raise AssertionError("version rejection diagnostic: %s" % rejected.stderr)

            tampered = dict(snapshot)
            tampered["messages"] = list(messages)
            tampered["messages"][-1] = dict(messages[-1])
            tampered["messages"][-1]["content"] = "tampered"
            legacy.write_text(compact(tampered))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "integrity" not in rejected.stderr:
                raise AssertionError("digest rejection diagnostic: %s" % rejected.stderr)

            orphan = json.loads(original_bytes)
            orphan["messages"][2]["tool_call_id"] = "orphan"
            resign(orphan)
            legacy.write_text(compact(orphan))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "orphan or mismatched" not in rejected.stderr:
                raise AssertionError("pairing rejection diagnostic: %s" % rejected.stderr)

            too_many = json.loads(original_bytes)
            too_many["messages"] = [{"role": "user", "content": "x"}] * 257
            resign(too_many)
            legacy.write_text(compact(too_many))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "message budget" not in rejected.stderr:
                raise AssertionError("message cap diagnostic: %s" % rejected.stderr)

            large_record = json.loads(original_bytes)
            large_record["messages"] = [{"role": "user", "content": "x" * (256 * 1024 + 1)}]
            resign(large_record)
            legacy.write_text(compact(large_record))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "record budget" not in rejected.stderr:
                raise AssertionError("record cap diagnostic: %s" % rejected.stderr)

            near_cap = json.loads(original_bytes)
            near_cap["messages"] = [
                {"role": role, "content": "x" * 3900}
                for _ in range(128) for role in ("user", "assistant")]
            resign(near_cap)
            legacy.write_text(compact(near_cap))
            restored = invoke(["-i", "--transport", "echo", "--no-tools", "--no-plugin"],
                              env, input_text="/resume %s\n/exit\n" % legacy)
            if "loaded 256 messages" not in restored.stdout:
                raise AssertionError("valid near-cap snapshot rejected: %s%s" %
                                     (restored.stdout, restored.stderr))

            legacy.write_bytes(b" " * (MAX_SNAPSHOT + 1))
            rejected = invoke(["-r", str(legacy)] + common, env, expected=2)
            if "byte budget" not in rejected.stderr:
                raise AssertionError("file cap diagnostic: %s" % rejected.stderr)
            if len(Provider.captures) != before:
                raise AssertionError("rejected snapshots reached the provider")

            # A failed/incomplete new turn cannot destroy the last valid file.
            invoke(["-p", "new turn", "--transport", "echo", "--no-tools",
                    "--no-plugin", "--max-steps", "1", "--save", str(snapshot_path)],
                   env, expected=1)
            if snapshot_path.read_bytes() != original_bytes:
                raise AssertionError("failed turn replaced the last valid snapshot")

            repl = invoke(["-i", "--transport", "echo", "--no-tools", "--no-plugin"],
                          env, input_text="/resume %s\n/exit\n" % snapshot_path)
            if "loaded 4 messages" not in repl.stdout:
                raise AssertionError("REPL full-turn resume: %s%s" %
                                     (repl.stdout, repl.stderr))
            test_repl_autosave(env)
        finally:
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=5)
    print("checkpoints: full-turn round-trip, injection/bounds, atomic save and REPL passed")


if __name__ == "__main__":
    main()
