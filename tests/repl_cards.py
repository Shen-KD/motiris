#!/usr/bin/env python3
"""REPL approval/config command and terminal-card regressions."""

import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
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
        "/approval nonsense", "/approval off", "/approval", "/config", "/exit",
    ])
    assert "thinking: on" in output and "thinking: off" in output, output
    assert "reasoning: high" in output, output
    assert "approval: off" in output, output
    assert "approval: ask" in output, output
    assert "usage: /approval [off|ask]" in output, output
    assert "stream: on" in output, output
    assert "max_steps: 0" in output, output
    assert "interactive terminal" in output, output


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
        initial = plain(repl.read_until("save changes"))
        for text in ("Session configuration", "stream: on", "approval: off",
                     "max_steps: 0", "thinking: off", "reasoning: provider",
                     "workspace_guard:", "save changes", "close"):
            assert text in initial, (text, initial)

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
            initial = plain(repl.read_until("allow for session"))
            assert "Tool approval" in initial and "shell" in initial, initial
            repl.send(keys)
            output = plain(repl.read_until("echo round complete: [ok]"))
            assert ("⚙ shell" in output) == should_execute, output
        finally:
            repl.close()


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-repl-cards-") as tmp:
        env = environment(Path(tmp))
        test_non_tty_commands(env)
        test_non_tty_approval_fails_closed(env)
        test_config_card(env, Path(tmp))
        test_approval_card(env)
    print("repl-cards: non-TTY commands, both cards and fail-closed approval passed")


if __name__ == "__main__":
    main()
