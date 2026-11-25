#!/usr/bin/env python3
"""REPL approval/config command and terminal-card regressions."""

import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import stat
import struct
import subprocess
import tempfile
import termios
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "motiris"
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")


def plain(data):
    if isinstance(data, (bytes, bytearray)):
        data = data.decode("utf-8", "replace")
    return ANSI.sub("", data).replace("\r", "")


def environment(home):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({
        "HOME": str(home),
        "MOTIRIS_HOME": str(home / "config"),
        "MOTIRIS_HISTFILE": str(home / "history"),
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
        "TERM": "xterm-256color",
    })
    (home / "config").mkdir(parents=True)
    return env


def run_pipe(env, commands):
    proc = subprocess.run(
        [str(BIN), "-i", "--transport", "echo", "--no-plugin"],
        input="\n".join(commands) + "\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=env, timeout=10)
    assert proc.returncode == 0, (proc.returncode, proc.stdout)
    return plain(proc.stdout)


def test_non_tty_commands(env):
    output = run_pipe(env, [
        "/thinking", "/thinking on", "/thinking off", "/reasoning high",
        "/reasoning", "/approval", "/approval ask", "/approval",
        "/approval nonsense", "/approval off", "/approval", "/config",
        "/model", "/exit",
    ])
    assert "thinking: on" in output and "thinking: off" in output, output
    assert "reasoning: high" in output, output
    assert "approval: off" in output, output
    assert "approval: ask" in output, output
    assert "usage: /approval [off|ask]" in output, output
    assert "stream: on" in output, output
    assert "max_steps: 0" in output, output
    assert "interactive terminal" in output, output
    assert "model_id: gpt-4o-mini" in output, output
    assert "think_mode: provider" in output and "max_tokens: 0" in output, output
    assert "max_retries: 0" in output and "retry_base_seconds: 1" in output, output
    assert "endpoint: https://api.openai.com/v1/chat/completions" in output, output
    assert "api_key: (not set)" in output, output


def test_model_summary_escapes_terminal_controls(env, home):
    home = home / "escape-summary"
    env = environment(home)
    malicious_model = "model\x1b]0;MODEL_PWN\x07"
    malicious_endpoint = "http://example.test/v1/chat/completions\x1b]0;URL_PWN\x07"
    (home / "config" / "config.json").write_text(json.dumps({
        "model": malicious_model, "base_url": malicious_endpoint,
    }))
    proc = subprocess.run(
        [str(BIN), "-i", "--transport", "echo", "--no-plugin", "-v"],
        input="/model\nhello\n/exit\n", text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, env=env, timeout=10)
    assert proc.returncode == 0, proc.stdout
    raw = proc.stdout.encode()
    assert b"\x1b]0;MODEL_PWN\x07" not in raw, raw
    assert b"\x1b]0;URL_PWN\x07" not in raw, raw


def test_model_editor_escapes_terminal_controls(env):
    malicious = "model\x1b]0;EDITOR_PWN\x07"
    repl = Repl(env, command=[
        str(BIN), "-i", "--transport", "echo", "--no-plugin", "-m", malicious,
    ])
    try:
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("/model\r\r")
        repl.read_until("Esc: cancel")
        assert b"\x1b]0;EDITOR_PWN\x07" not in bytes(repl.output), bytes(repl.output)
        repl.send("\x1bq")
        repl.read_until("motiris> ")
        repl.send("/exit\r")
        repl.read_until("bye")
    finally:
        repl.close()


def test_non_tty_approval_fails_closed(env):
    output = run_pipe(env, ["/approval ask", "run a tool", "/exit"])
    assert "approval required" in output, output
    assert "echo round complete" not in output, output
    assert "motiris works" not in output, output


class Repl:
    def __init__(self, env, extra_args=(), command=None):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, 100, 0, 0))
        argv = command or [str(BIN), "-i", "--transport", "echo", "--no-plugin",
                           *extra_args]
        # setsid(1) performs setsid + TIOCSCTTY without Python preexec_fn,
        # which can deadlock when the test driver also runs provider threads.
        self.proc = subprocess.Popen(
            ["setsid", "--ctty", *argv],
            stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
        os.close(slave)
        self.output = bytearray()

    def read_until(self, token, timeout=6):
        deadline = time.monotonic() + timeout
        answered = 0
        while time.monotonic() < deadline:
            if isinstance(token, bytes):
                found = token in self.output
            elif hasattr(token, "search"):
                found = token.search(plain(self.output)) is not None
            else:
                found = token in plain(self.output)
            if found:
                return bytes(self.output)
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            self.output.extend(chunk)
            while self.output.count(b"\x1b[6n") > answered:
                os.write(self.master, b"\x1b[1;1R")
                answered += 1
        raise AssertionError("PTY did not produce %r (process=%r): %s" %
                             (token, self.proc.poll(), plain(self.output)[-5000:]))

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master, data)

    def clear(self):
        self.output.clear()

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)
        os.close(self.master)


def test_config_card(env, home):
    repl = Repl(env)
    try:
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("/config\r")
        initial = plain(repl.read_until("keep session values"))
        for text in ("Session configuration", "stream: on", "approval: off",
                     "max_steps: 0", "thinking: off", "reasoning: provider",
                     "workspace_guard:", "save changes", "close"):
            assert text in initial, (text, initial)
        repl.clear()
        time.sleep(0.35)
        ready, _, _ = select.select([repl.master], [], [], 0)
        if ready:
            repl.output.extend(os.read(repl.master, 65536))
        assert not ready, "idle config card redrawn: " + plain(repl.output)

        # Cycle unlimited -> 5, then toggle stream off. Changes are immediate.
        repl.send("\x1b[B\x1b[B\r")
        repl.read_until(re.compile(r"max_steps:\s+5\b"))
        repl.send("\x1b[A\x1b[A\r")
        repl.read_until(re.compile(r"stream:\s+off\b"))
        repl.clear()
        repl.send("\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\r")  # save changes
        repl.read_until("configuration saved")
        repl.read_until("motiris> ")
        saved = json.loads((home / "config" / "config.json").read_text())
        assert saved["stream"] is False, saved
        assert saved["max_steps"] == 5 and saved["workspace_guard"] is False, saved

        # Right/Left navigate too. q closes without undoing approval=ask.
        repl.clear()
        repl.send("/config\r")
        repl.read_until("save changes")
        repl.send("\x1b[C\r")
        repl.read_until(re.compile(r"approval:\s+ask"))
        repl.send("\x1b[D")
        repl.clear()
        repl.send("q")
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("/approval\r")
        assert re.search(r"approval:\s+ask", plain(repl.read_until(re.compile(r"approval:\s+ask"))))
        repl.read_until("motiris> ")

        # Escape and ISIG Ctrl+C both leave the card and preserve the REPL.
        for key in ("\x1b", "\x03"):
            repl.clear()
            repl.send("/config\r")
            repl.read_until("save changes")
            repl.clear()
            repl.send(key)
            repl.read_until("motiris> ")
            assert repl.proc.poll() is None

        repl.send("/exit\r")
        repl.read_until("bye")
        assert repl.proc.wait(timeout=5) == 0
    finally:
        repl.close()


class ModelProvider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ModelHandler)
        self.requests = []
        self.status = 200
        self.models = [{"id": "model-alpha"}, {"id": "org/exact:model"}]


class ModelHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        body_in = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append({
            "path": self.path, "body": body_in,
            "authorization": self.headers.get_all("Authorization") or [],
        })
        body = json.dumps({"data": self.server.models}).encode()
        self.send_response(self.server.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def test_model_card_idle_does_not_redraw(env):
    repl = Repl(env)
    try:
        repl.read_until("motiris> ")
        repl.send("/model\r")
        repl.read_until("session changes are immediate; close asks to save")
        repl.clear()
        time.sleep(0.35)  # several raw-read timeouts; no key was pressed
        ready, _, _ = select.select([repl.master], [], [], 0)
        if ready:
            repl.output.extend(os.read(repl.master, 65536))
        assert not ready, "idle card redrawn: " + plain(repl.output)
        repl.send("\r")
        repl.read_until("Esc: cancel")
        repl.clear()
        time.sleep(0.35)
        ready, _, _ = select.select([repl.master], [], [], 0)
        if ready:
            repl.output.extend(os.read(repl.master, 65536))
        assert not ready, "idle editor redrawn: " + plain(repl.output)
        repl.send("\r")  # accept unchanged text: still no semantic edit
        repl.read_until("model unchanged")
        repl.send("q")  # no edits, so no save prompt
        repl.read_until("motiris> ")
        repl.send("/exit\r")
        repl.read_until("bye")
    finally:
        repl.close()


def test_model_card_expanded_choices(env, home):
    home = home / "expanded-choices"
    env = environment(home)
    provider = ModelProvider()
    provider.models = [{"id": "model-%02d" % i} for i in range(11)]
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    endpoint = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port
    repl = Repl(env, command=[
        str(BIN), "-i", "--transport", "libcurl", "--no-plugin",
        "-b", endpoint,
    ])
    try:
        repl.read_until("motiris> ")
        repl.send("/model\r")
        repl.read_until("keep session values")
        repl.send("\x1b[B" * 8 + "\r")
        repl.read_until("11 models loaded")
        repl.send("\x1b[A" * 8 + "\r")
        expanded = plain(repl.read_until("up/down: navigate"))
        assert "model-00" in expanded and "model-07" in expanded, expanded
        assert "model-10" not in expanded, expanded
        # Up from the first item wraps to the last and scrolls the window.
        repl.send("\x1b[A")
        scrolled = plain(repl.read_until("showing 4-11 of 11"))
        assert "model-10" in scrolled, scrolled
        repl.send("\r")
        repl.read_until(re.compile(r"model_id:\s+model-10"))

        # Escape collapses the picker without applying the highlighted change.
        repl.send("\r")
        repl.read_until("up/down: navigate")
        repl.send("\x1b[A")
        repl.clear()
        repl.send("\x1b")
        cancelled = plain(repl.read_until("selection cancelled"))
        final_frame = cancelled[cancelled.rfind("Model configuration"):]
        assert re.search(r"model_id:\s+model-10", final_frame), final_frame
        assert "model-09" not in final_frame, final_frame

        # Manual editing remains available even after a list was fetched.
        repl.send("e\x15manual-after-list\r")
        repl.read_until(re.compile(r"model_id:\s+manual-after-list"))
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.clear()
        repl.send("\x1b")  # return to the card without closing
        repl.read_until("model updated for this session")
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("n")
        repl.clear()
        repl.read_until("motiris> ")
        assert not (home / "config" / "config.json").exists()
        # Declining save does not clear the REPL-scoped dirty state: reopening
        # and closing again must still offer a save without another edit.
        repl.send("/model\r")
        repl.read_until("session changes are immediate; close asks to save")
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("n")
        repl.clear()
        repl.read_until("motiris> ")
        repl.send("/exit\r")
        repl.read_until("bye")
    finally:
        repl.close()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)


def test_model_card(env, home):
    provider = ModelProvider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    secret = "card-secret-sentinel"
    endpoint = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port
    repl = Repl(env, command=[
        str(BIN), "-i", "--transport", "libcurl", "--no-plugin",
        "-b", "http://127.0.0.1:1/v1/chat/completions",
    ])
    try:
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("/model\r")
        initial = plain(repl.read_until("keep session values"))
        for text in ("Model configuration", "model_id:", "gpt-4o-mini",
                     "think_mode:", "provider", "max_tokens:", "0",
                     "max_retries:", "0", "retry_base_s:", "1",
                     "reasoning:", "endpoint:", "api_key:", "(not set)",
                     "refresh models", "close"):
            assert text in initial, (text, initial)
        assert "save changes" not in initial, initial
        assert provider.requests == [], "opening /model performed network I/O"

        # Every text field is edited inside the raw card, not through linenoise.
        repl.send("\r\x15manual-model\r")
        repl.read_until(re.compile(r"model_id:\s+manual-model"))
        repl.send("\x1b[B\r")  # think_mode provider -> on
        repl.read_until(re.compile(r"think_mode:\s+on\b"))
        repl.send("\x1b[B\r\x154096\r")
        repl.read_until(re.compile(r"max_tokens:\s+4096\b"))
        repl.send("\x1b[B\r\x152\r")
        repl.read_until(re.compile(r"max_retries:\s+2\b"))
        repl.send("\x1b[B\r\x150\r")
        repl.read_until(re.compile(r"retry_base_s:\s+0\b"))
        repl.send("\x1b[B\r")  # reasoning provider -> low
        repl.read_until(re.compile(r"reasoning:\s+low\b"))
        repl.send("\x1b[B\r\x15" + endpoint + "\r")
        repl.read_until(re.compile(r"endpoint:\s+http://127\.0\.0\.1:"))
        repl.send("\x1b[B\r" + secret + "\r")
        masked = plain(repl.read_until(re.compile(r"api_key:\s+\(set\)")))
        assert secret not in masked, masked
        assert provider.requests == [], "editing fields performed network I/O"

        # Declining save keeps the session credential pending: a later explicit
        # save must still persist it without retyping.
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("n")
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("/model\r")
        repl.read_until("keep session values")
        assert provider.requests == [], "reopening /model performed network I/O"

        # Only the explicit refresh action performs one authenticated GET.
        repl.send("\x1b[B" * 8 + "\r")
        refreshed = plain(repl.read_until("2 models loaded"))
        assert secret not in refreshed
        assert len(provider.requests) == 1
        assert provider.requests[0]["path"] == "/v1/models"
        assert provider.requests[0]["body"] == b""
        assert provider.requests[0]["authorization"] == ["Bearer " + secret]

        # Enter expands the fetched choices; down+Enter selects the exact ID.
        repl.send("\x1b[A" * 8 + "\r")
        expanded = plain(repl.read_until("up/down: navigate"))
        assert "model-alpha" in expanded and "org/exact:model" in expanded, expanded
        repl.send("\x1b[B\r")
        repl.read_until(re.compile(r"model_id:\s+org/exact:model"))
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("y")
        repl.read_until("model configuration saved")
        repl.read_until("motiris> ")
        saved = json.loads((home / "config" / "config.json").read_text())
        assert saved["model"] == "org/exact:model", saved
        assert saved["think_mode"] == "on" and saved["max_tokens"] == 4096, saved
        assert saved["max_retries"] == 2 and saved["retry_base_seconds"] == 0, saved
        assert saved["reasoning_effort"] == "low", saved
        assert saved["base_url"] == endpoint and saved["api_key_env"] == "MOTIRIS_API_KEY", saved
        assert secret not in json.dumps(saved)
        env_path = home / "config" / "env"
        assert ("MOTIRIS_API_KEY=" + secret) in env_path.read_text()
        assert stat.S_IMODE(env_path.stat().st_mode) == 0o600
        assert secret not in plain(repl.output)

        # Reopening is idle; a failed manual refresh is sanitized and bounded.
        before = len(provider.requests)
        repl.clear()
        repl.send("/model\r")
        repl.read_until("keep session values")
        assert len(provider.requests) == before
        provider.status = 401
        repl.send("\x1b[B" * 8 + "\r")
        failed = plain(repl.read_until("refresh failed: http status 401"))
        assert secret not in failed and len(failed) < 10000
        repl.send("q")
        repl.read_until("motiris> ")
        repl.send("/exit\r")
        repl.read_until("bye")
        assert repl.proc.wait(timeout=5) == 0
        assert secret not in (home / "history").read_text()
    finally:
        repl.close()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)

    # A new process reloads model settings and the mode-0600 env credential.
    output = run_pipe(env, ["/model", "/exit"])
    assert "model_id: org/exact:model" in output, output
    assert "think_mode: on" in output and "max_tokens: 4096" in output, output
    assert "max_retries: 2" in output and "retry_base_seconds: 0" in output, output
    assert "reasoning: low" in output and "api_key: (set)" in output, output
    assert secret not in output


def test_model_card_interrupt_close_does_not_loop(env, home):
    home = home / "interrupt-close"
    env = environment(home)
    repl = Repl(env)
    try:
        repl.read_until("motiris> ")
        repl.send("/model\r")
        repl.read_until("session changes are immediate; close asks to save")
        repl.send("\x1b[B\r")  # dirty: provider -> on
        repl.read_until(re.compile(r"think_mode:\s+on\b"))
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.clear()
        repl.send("\x03")
        repl.read_until("motiris> ", timeout=2)
        assert repl.proc.poll() is None
        assert not (home / "config" / "config.json").exists()
        repl.send("/exit\r")
        repl.read_until("bye")
    finally:
        repl.close()


def test_model_card_failed_save_retains_key(env, home):
    secret = "retry-secret-sentinel"
    repl = Repl(env)
    config = home / "config"
    try:
        repl.read_until("motiris> ")
        repl.send("/model\r")
        repl.read_until("keep session values")
        repl.send("\x1b[B" * 7 + "\r" + secret + "\r")
        repl.read_until(re.compile(r"api_key:\s+\(set\)"))
        config_json = config / "config.json"
        if config_json.exists():
            config_json.unlink()
        config_json.mkdir()  # deterministic regular-file rejection, even for UID 0
        repl.clear()
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("y")
        failed = plain(repl.read_until("values could not be saved"))
        assert secret not in failed
        assert ("MOTIRIS_API_KEY=" + secret) in (config / "env").read_text()
        # The card stays open after failure; fix the path and retry without
        # reopening or retyping the credential.
        config_json.rmdir()
        repl.send("q")
        repl.read_until("Save configuration before closing?")
        repl.send("y")
        repl.read_until("model configuration saved")
        assert ("MOTIRIS_API_KEY=" + secret) in (config / "env").read_text()
        saved = json.loads((config / "config.json").read_text())
        assert saved["api_key_env"] == "MOTIRIS_API_KEY", saved
        repl.read_until("motiris> ")
        repl.send("/exit\r")
        repl.read_until("bye")
    finally:
        if (config / "config.json").is_dir():
            (config / "config.json").rmdir()
        repl.close()


def test_approval_card(env):
    # Use one real terminal per decision. Cross-turn grants, /new isolation and
    # allow-once repetition are covered by the direct strict core-nofs tests.
    for keys, should_execute in [("\r", True), ("\x1b[B\r", True),
                                 ("\x1b[B\x1b[B\r", False)]:
        repl = Repl(env)
        try:
            repl.read_until("motiris> ")
            repl.send("/approval ask\r")
            repl.read_until(re.compile(r"approval:\s+ask"))
            repl.clear()
            repl.send("run a tool\r")
            initial = plain(repl.read_until("q/Esc: deny"))
            assert "Tool approval" in initial and "shell" in initial, initial
            repl.clear()
            time.sleep(0.35)
            ready, _, _ = select.select([repl.master], [], [], 0)
            if ready:
                repl.output.extend(os.read(repl.master, 65536))
            assert not ready, "idle approval card redrawn: " + plain(repl.output)
            repl.send(keys)
            output = plain(repl.read_until("echo round complete: [ok]"))
            assert ("⚙ shell" in output) == should_execute, output
        finally:
            repl.close()


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-repl-cards-") as tmp:
        env = environment(Path(tmp))
        test_non_tty_commands(env)
        test_model_summary_escapes_terminal_controls(env, Path(tmp))
        test_model_editor_escapes_terminal_controls(env)
        test_non_tty_approval_fails_closed(env)
        test_config_card(env, Path(tmp))
        test_model_card_idle_does_not_redraw(env)
        test_model_card_expanded_choices(env, Path(tmp))
        test_model_card(env, Path(tmp))
        test_model_card_interrupt_close_does_not_loop(env, Path(tmp))
        test_model_card_failed_save_retains_key(env, Path(tmp))
        test_approval_card(env)
    print("repl-cards: non-TTY commands, config/model cards and fail-closed approval passed")


if __name__ == "__main__":
    main()
