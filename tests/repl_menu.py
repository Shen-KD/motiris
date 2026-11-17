#!/usr/bin/env python3
"""Real-PTY regression for live slash-command lists in the REPL."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def plain(data):
    return ANSI.sub("", data.decode("utf-8", "replace")).replace("\r", "")


class Repl:
    def __init__(self, home):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MOTIRIS_")}
        env.update({
            "HOME": str(home),
            "MOTIRIS_HOME": str(home / "config"),
            "MOTIRIS_HISTFILE": str(home / "history"),
        })
        (home / "config").mkdir()
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 100, 0, 0))
        self.proc = subprocess.Popen(
            [str(ROOT / "motiris"), "-i", "--transport", "echo", "--no-plugin"],
            stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
        os.close(slave)

    def read_until(self, token, timeout=5):
        data = bytearray()
        answered_cursor_queries = 0
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            data.extend(chunk)
            # linenoise asks for the cursor position before drawing a prompt.
            while data.count(b"\x1b[6n") > answered_cursor_queries:
                os.write(self.master, b"\x1b[1;1R")
                answered_cursor_queries += 1
            if token in plain(data):
                return bytes(data)
        raise AssertionError("PTY did not produce %r: %s" %
                             (token, plain(data)[-4000:]))

    def send(self, text):
        os.write(self.master, text.encode())

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)
        os.close(self.master)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        repl = Repl(Path(tmp))
        try:
            repl.read_until("motiris> ")

            # No Tab: one slash must immediately list every command.
            repl.send("/")
            output = plain(repl.read_until("leave the repl"))
            for command in ("/help", "/new", "/tools", "/sessions",
                            "/resume", "/skills", "/exit"):
                assert command in output, (command, output)
            assert "/quit" not in output, output

            # Prefix searches repaint the list on each inserted character.
            repl.send("s")
            output = plain(repl.read_until("list available skills [TERM]"))
            assert "/sessions" in output, output
            assert "/skills" in output, output
            assert "/help" not in output, output

            repl.send("e")
            output = plain(repl.read_until("review past session logs [TERM]"))
            assert "/sessions" in output, output
            assert "/skills" not in output, output

            repl.send("\x15")  # Ctrl-U: dismiss the search and its menu.
            cleared = repl.read_until("motiris> ")
            assert b"\x1b[J" in cleared, cleared

            # The removed alias must not quit or become a model turn.
            repl.send("/quit\r")
            output = plain(repl.read_until("unknown command: /quit"))
            assert "echo round complete" not in output, output

            repl.send("/exit\r")
            repl.read_until("bye")
            assert repl.proc.wait(timeout=5) == 0
        finally:
            repl.close()
    print("repl-menu: live slash list and prefix filtering passed")


if __name__ == "__main__":
    main()
