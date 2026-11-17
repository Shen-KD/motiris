#!/usr/bin/env python3
"""Real-PTY regression: interrupt a slow model stream and continue the chat."""

import json
import os
import pty
import re
import select
import signal
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "motiris")
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append(json.loads(body))
        call = len(self.server.requests)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def delta(text):
            payload = {"choices": [{"delta": {"content": text}}]}
            self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())
            self.wfile.flush()

        try:
            if call == 1:
                delta("partial-answer")
                # Keep the first response open until the client cancellation
                # closes the connection. Extra deltas must not reach history.
                for _ in range(300):
                    time.sleep(0.05)
                    delta("-discarded")
            else:
                delta("continued-answer")
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Repl:
    def __init__(self, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(BIN, [BIN, "-i", "--no-tools", "--no-plugin",
                            "--max-steps", "1", "--transport", "libcurl",
                            "--model", "cancel-test", "--base-url", env["URL"]],
                      env)
        self.output = bytearray()

    def read_until(self, needle, timeout=8, suffix=False):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = self.plain()
            if (text.endswith(needle) if suffix else needle in text):
                return text
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                break
            if not data:
                break
            self.output.extend(data)
            # Reply to linenoise's cursor-position queries.
            for _ in range(data.count(b"\x1b[6n")):
                os.write(self.fd, b"\x1b[1;1R")
        raise AssertionError("PTY did not produce %r: %s" % (needle, self.plain()))

    def plain(self):
        return ANSI.sub("", self.output.decode(errors="replace"))

    def send(self, data):
        os.write(self.fd, data.encode())

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if not pid:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
        except (ChildProcessError, ProcessLookupError):
            pass


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    home = tempfile.TemporaryDirectory(prefix="motiris-repl-cancel-")
    repl = None
    try:
        env = os.environ.copy()
        env.update({
            "HOME": home.name,
            "MOTIRIS_HOME": home.name,
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
            "TERM": "xterm-256color",
            "LINENOISE_COLS": "80",
            "URL": "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port,
        })
        env.pop("MOTIRIS_API_KEY", None)
        os.makedirs(env["MOTIRIS_HOME"], exist_ok=True)
        repl = Repl(env)
        repl.read_until("motiris> ", suffix=True)
        repl.output.clear()
        repl.send("first instruction\r")
        repl.read_until("partial-answer")
        repl.send("\x03")
        cancelled = repl.read_until("generation cancelled")
        assert "second" not in cancelled
        repl.read_until("motiris> ", suffix=True)
        repl.output.clear()

        repl.send("second instruction\r")
        repl.read_until("continued-answer")
        repl.read_until("motiris> ", suffix=True)
        assert repl.plain().count("continued-answer") == 1, repl.plain()
        repl.send("/exit\r")
        end = time.monotonic() + 10
        while time.monotonic() < end:
            # Drain exit diagnostics as well; otherwise a sanitizer report can
            # fill the PTY buffer and prevent the child from exiting.
            while select.select([repl.fd], [], [], 0)[0]:
                try:
                    data = os.read(repl.fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                repl.output.extend(data)
            try:
                pid, status = os.waitpid(repl.pid, os.WNOHANG)
                if pid:
                    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (
                        status, repl.plain())
                    break
            except ChildProcessError:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("REPL did not exit after continued answer")

        deadline = time.monotonic() + 2
        while len(provider.requests) < 2 and time.monotonic() < deadline:
            time.sleep(0.02)
        assert len(provider.requests) == 2, len(provider.requests)
        followup = provider.requests[1]["messages"]
        users = [m.get("content") for m in followup if m.get("role") == "user"]
        assert users == ["first instruction", "second instruction"], users
        assert "partial-answer" not in json.dumps(followup)
        assert "-discarded" not in json.dumps(followup)

        session_dir = os.path.join(home.name, ".local/share/motiris/sessions")
        logs = [os.path.join(session_dir, name) for name in os.listdir(session_dir)
                if name.endswith(".log")]
        assert len(logs) == 1, logs
        with open(logs[0], encoding="utf-8") as saved_file:
            saved = saved_file.read()
        assert "first instruction" in saved and "second instruction" in saved
        assert "partial-answer" not in saved and "-discarded" not in saved
        print("repl-cancel: interrupt, history rollback and continued instruction passed")
    finally:
        if repl:
            repl.close()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=2)
        home.cleanup()


if __name__ == "__main__":
    main()
