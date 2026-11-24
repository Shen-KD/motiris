#!/usr/bin/env python3
"""Real-PTY generation-time editing, FIFO continuation and interruption."""

import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.condition = threading.Condition()
        self.release_first = False
        self.release_third = False
        self.release_buffered_flag = False

    def wait_release(self, third=False):
        with self.condition:
            while not (self.release_third if third else self.release_first):
                self.condition.wait(timeout=15)
                if not (self.release_third if third else self.release_first):
                    return

    def release(self, third=False):
        with self.condition:
            if third:
                self.release_third = True
            else:
                self.release_first = True
            self.condition.notify_all()

    def wait_buffered(self):
        with self.condition:
            while not self.release_buffered_flag:
                self.condition.wait(timeout=15)
                if not self.release_buffered_flag:
                    return

    def release_buffered(self):
        with self.condition:
            self.release_buffered_flag = True
            self.condition.notify_all()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        request = json.loads(body)
        self.server.requests.append(request)
        call = len(self.server.requests)
        if call == 7:
            self.server.wait_buffered()
            body = json.dumps({"choices": [{
                "finish_reason": "stop",
                "message": {"role": "assistant", "content": "buffered-visible"},
            }]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def delta(text):
            event = {"choices": [{"delta": {"content": text}}]}
            self.wfile.write(b"data: " + json.dumps(event).encode() + b"\n\n")
            self.wfile.flush()

        try:
            if call == 1:
                delta("first-working")
                self.server.wait_release()
                delta("-first-done")
            elif call == 2:
                delta("second-answer")
            elif call == 3:
                delta("third-partial")
                self.server.wait_release(third=True)
                delta("-third-discarded")
            elif call == 4:
                delta("queued-after-cancel")
            elif call == 5:
                delta("draft-answer")
            else:
                delta("screen-one")
                delta("-screen-two")
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Repl:
    def __init__(self, env, extra_args=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(str(BIN), [str(BIN), "-i", "--no-tools", "--no-plugin",
                                 "--max-steps", "1", "--transport", "libcurl",
                                 "--model", "generation-input", "--base-url",
                                 env["URL"], *extra_args], env)
        self.output = bytearray()

    def plain(self):
        return ANSI.sub("", self.output.decode(errors="replace")).replace("\r", "")

    def visible_lines(self):
        """Apply the cursor/erasure sequences used by linenoise to the screen."""
        data = self.output.decode(errors="replace")
        rows = {}
        row = col = 0
        i = 0
        while i < len(data):
            if data[i] == "\x1b" and i + 1 < len(data) and data[i + 1] == "[":
                match = re.match(r"\x1b\[([0-9;?]*)([@-~])", data[i:])
                if match:
                    params, final = match.groups()
                    if params.startswith("?"):
                        i += len(match.group(0))
                        continue
                    amount = int(params.split(";")[0] or "1")
                    if final == "A":
                        row = max(0, row - amount)
                    elif final == "B":
                        row += amount
                    elif final == "C":
                        col += amount
                    elif final == "D":
                        col = max(0, col - amount)
                    elif final == "K":
                        line = rows.get(row, {})
                        if params in ("", "0"):
                            line = {key: value for key, value in line.items()
                                    if key < col}
                        elif params == "1":
                            line = {key: value for key, value in line.items()
                                    if key > col}
                        elif params == "2":
                            line = {}
                        rows[row] = line
                    i += len(match.group(0))
                    continue
            if data[i] == "\r":
                col = 0
            elif data[i] == "\n":
                row += 1
            elif data[i] >= " ":
                rows.setdefault(row, {})[col] = data[i]
                col += 1
            i += 1
        return ["".join(rows.get(r, {}).get(c, " ")
                       for c in range(max(rows.get(r, {}), default=-1) + 1))
                for r in range(max(rows) + 1)] if rows else []

    def read_until(self, needle, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.plain():
                return self.plain()
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.output.extend(chunk)
            for _ in range(chunk.count(b"\x1b[6n")):
                os.write(self.fd, b"\x1b[1;1R")
        raise AssertionError("PTY did not produce %r: %s" %
                             (needle, self.plain()[-6000:]))

    def send(self, data):
        os.write(self.fd, data.encode())

    def wait_exit(self, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while select.select([self.fd], [], [], 0)[0]:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    break
                self.output.extend(chunk)
            try:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return
            if pid:
                assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (
                    status, self.plain())
                return
            time.sleep(0.05)
        raise AssertionError("REPL did not exit: " + self.plain()[-4000:])

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if not pid:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
        except (ChildProcessError, ProcessLookupError):
            pass


def users(request):
    return [m.get("content") for m in request["messages"]
            if m.get("role") == "user"]


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    home = tempfile.TemporaryDirectory(prefix="motiris-generation-input-")
    repl = None
    try:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MOTIRIS_")}
        env.update({
            "HOME": home.name,
            "MOTIRIS_HOME": home.name,
            "MOTIRIS_HISTFILE": str(Path(home.name) / "history"),
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
            "TERM": "xterm-256color",
            "LINENOISE_COLS": "100",
            "URL": "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port,
        })
        repl = Repl(env)
        repl.read_until("motiris> ")
        repl.output.clear()
        repl.send("first instruction\r")
        repl.read_until("first-working")

        # Enter queues complete lines without cancelling the active response.
        repl.send("second instruction\r")
        repl.read_until("queued 1")
        repl.send("/reasoning high\r")
        repl.read_until("queued 2")
        repl.send("third instruction\r")
        repl.read_until("queued 3")
        repl.send("fourth instruction\r")
        repl.read_until("queued 4")
        time.sleep(0.3)
        assert len(provider.requests) == 1, len(provider.requests)
        assert "generation cancelled" not in repl.plain(), repl.plain()
        assert users(provider.requests[0]) == ["first instruction"]

        # Leave an edited, unsubmitted draft in the same prompt while queued
        # turns run. Left/Backspace-style editing must reach the line editor.
        repl.send("unfinished drft")
        repl.send("\x1b[D\x1b[D")
        repl.send("a")
        repl.read_until("unfinished draft")

        provider.release()
        repl.read_until("second-answer")
        repl.read_until("reasoning: high")
        repl.read_until("third-partial")
        deadline = time.monotonic() + 2
        while len(provider.requests) < 3 and time.monotonic() < deadline:
            time.sleep(0.02)
        assert len(provider.requests) == 3, len(provider.requests)
        assert users(provider.requests[1]) == ["first instruction",
                                               "second instruction"]
        assert users(provider.requests[2]) == ["first instruction",
                                               "second instruction",
                                               "third instruction"]

        # A lone Escape is an explicit interrupt, not an arrow prefix. The
        # already queued fourth turn runs next while the separate draft stays
        # unsubmitted and intact.
        repl.output.clear()
        repl.send("\x1b")
        repl.read_until("generation cancelled")
        # Cancellation is printed before the preserved draft is redrawn; wait
        # for that separate terminal update instead of assuming one read has
        # already captured both writes.
        repl.read_until("unfinished draft")
        repl.read_until("queued-after-cancel")
        deadline = time.monotonic() + 2
        while len(provider.requests) < 4 and time.monotonic() < deadline:
            time.sleep(0.02)
        assert len(provider.requests) == 4, len(provider.requests)
        assert users(provider.requests[3])[-1] == "fourth instruction", users(
            provider.requests[3])
        serialized = json.dumps(provider.requests[3])
        assert "third-partial" not in serialized and "-third-discarded" not in serialized

        repl.output.clear()
        repl.send("\r")
        repl.read_until("draft-answer")
        repl.read_until("motiris> ")
        deadline = time.monotonic() + 2
        while len(provider.requests) < 5 and time.monotonic() < deadline:
            time.sleep(0.02)
        assert len(provider.requests) == 5, len(provider.requests)
        assert users(provider.requests[4])[-1] == "unfinished draft", users(
            provider.requests[4])

        repl.send("/exit\r")
        repl.wait_exit()
        visible = "\n".join(repl.visible_lines())
        assert "draft-answer" in visible, visible

        # With no concurrent editing, consecutive deltas must remain one
        # visible answer after the final prompt and exit redraws.
        repl.close()
        repl = Repl(env, ("-v",))
        repl.read_until("motiris> ")
        repl.output.clear()
        repl.send("screen instruction\r")
        repl.read_until("-screen-two")
        repl.send("/exit\r")
        repl.wait_exit()
        lines = repl.visible_lines()
        visible = "\n".join(lines)
        assert "screen-one-screen-two" in visible, visible
        assert "screen-one-screen-two[motiris]" not in visible, visible
        assert any(line.startswith("[motiris] response:") for line in lines), visible

        # Buffered verbose diagnostics are emitted while the idle prompt is
        # visible. They must use the same serialized renderer as the answer,
        # not write directly over the prompt from the worker.
        repl.close()
        repl = Repl(env, ("-v", "--no-stream"))
        repl.read_until("motiris> ")
        repl.output.clear()
        repl.send("buffered screen\r")
        repl.read_until("motiris> ")
        deadline = time.monotonic() + 2
        while len(provider.requests) < 7 and time.monotonic() < deadline:
            time.sleep(0.02)
        assert len(provider.requests) == 7, len(provider.requests)
        provider.release_buffered()
        repl.read_until("buffered-visible")
        repl.send("/exit\r")
        repl.wait_exit()
        lines = repl.visible_lines()
        visible = "\n".join(lines)
        assert "buffered-visible" in visible, visible
        assert "motiris> [motiris] response" not in visible, visible
        assert any(line.startswith("[motiris] response:") for line in lines), visible
        print("repl-generation-input: live editing, FIFO, Esc interrupt and draft preservation passed")
    finally:
        provider.release()
        provider.release(third=True)
        provider.release_buffered()
        if repl:
            repl.close()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=2)
        home.cleanup()


if __name__ == "__main__":
    main()
