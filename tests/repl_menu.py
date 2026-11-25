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


def terminal_lines(data):
    """Render the ASCII screen updates used by the menu, preserving columns."""
    rows = {}
    row = col = i = 0
    while i < len(data):
        c = data[i]
        if c == 0x1b and i + 1 < len(data) and data[i + 1] == ord("["):
            match = re.match(rb"\x1b\[([0-9;?]*)([@-~])", data[i:])
            if match:
                params, final = match.groups()
                amount = int(params.split(b";")[0] or b"1")
                if final == ord("A"):
                    row = max(0, row - amount)
                elif final == ord("B"):
                    row += amount
                elif final == ord("C"):
                    col += amount
                elif final == ord("D"):
                    col = max(0, col - amount)
                i += len(match.group(0))
                continue
        if c == ord("\r"):
            col = 0
        elif c == ord("\n"):
            row += 1
        elif c >= 32:
            rows.setdefault(row, {})[col] = chr(c)
            col += 1
        i += 1
    return ["".join(rows[r].get(c, " ") for c in range(max(rows[r]) + 1))
            for r in range(max(rows) + 1)] if rows else []


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
            if (token in data if isinstance(token, bytes)
                    else token in plain(data)):
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
            raw = repl.read_until("leave the repl")
            output = plain(raw)
            lines = terminal_lines(raw)
            for command in ("/help", "/new", "/tools", "/sessions",
                            "/load", "/save", "/resume", "/skills", "/thinking",
                            "/reasoning", "/approval", "/config", "/model",
                            "/hotkey", "/exit"):
                assert command in output, (command, output)
                assert any(line.startswith("  " + command) for line in lines), (
                    command, lines)
            assert "/quit" not in output, output
            assert b"\x1b[7m  /help" in raw, raw

            # Arrows move the highlight without changing the typed prefix.
            repl.send("\x1b[B")
            raw = repl.read_until("leave the repl")
            assert b"\x1b[7m  /new" in raw, raw
            assert b"\x1b[7m  /help" not in raw, raw

            # Typing keeps the original prefix and resets to its first match.
            repl.send("s")
            raw = repl.read_until("list available skills [TERM]")
            output = plain(raw)
            assert "/sessions" in output, output
            assert "/skills" in output, output
            assert "/help" not in output, output
            assert b"\x1b[7m  /sessions" in raw, raw

            # Every edit, including transposition, resets the selected match.
            repl.send("\x1b[B")
            raw = repl.read_until("list available skills [TERM]")
            assert b"\x1b[7m  /save" in raw, raw
            repl.send("\x1b[D")
            repl.read_until("list available skills [TERM]")
            repl.send("\x14")
            repl.read_until(b"\x1b[J\x1b[1A\r\x1b[11C")
            repl.send("\x1b[D")
            repl.read_until(b"\r\x1b[10C")
            repl.send("\x14")
            raw = repl.read_until("list available skills [TERM]")
            assert b"\x1b[7m  /sessions" in raw, raw

            repl.send("e")
            raw = repl.read_until("review past session logs [TERM]")
            output = plain(raw)
            assert "/sessions" in output, output
            assert "/skills" not in output, output

            repl.send("\x15")  # Ctrl-U: dismiss the search and its menu.
            cleared = repl.read_until("motiris> ")
            assert b"\x1b[J" in cleared, cleared

            # The removed alias must not quit or become a model turn.
            repl.send("/quit\r")
            output = plain(repl.read_until("unknown command: /quit"))
            assert "echo round complete" not in output, output

            # A no-match placeholder is not selectable: arrows still use history.
            repl.send("/zzz")
            repl.read_until(b"no matching commands (/help)\x1b[1A\r\x1b[13C")
            repl.send("\x1b[A")
            raw = repl.read_until(b"no matching commands (/help)\x1b[1A\r")
            output = plain(raw)
            assert "motiris> /quit" in output, output
            repl.send("\x15")
            repl.read_until("motiris> ")

            # Idle Escape dismisses Tab completion without applying the
            # highlighted command to the next typed character.
            repl.send("/s")
            repl.read_until("list available skills [TERM]")
            repl.send("\t")
            repl.read_until("motiris> /sessions")
            repl.send("\x1b")
            # The dismissal redraw restores the two-column `/s` cursor.
            repl.read_until(b"\r\x1b[11C")
            repl.send("x\r")
            output = plain(repl.read_until("unknown command: /sx"))
            assert "/sessionsx" not in output, output

            repl.send("/hotkey\r")
            output = plain(repl.read_until("clear the screen"))
            for text in ("interactive keys", "select a command", "enter",
                         "tab", "ctrl+c", "ctrl+d", "ctrl+u"):
                assert text in output, (text, output)

            # Up from the first item wraps to the last; Enter accepts it.
            repl.send("/")
            repl.read_until("leave the repl")
            repl.send("\x1b[A")
            raw = repl.read_until("leave the repl")
            assert b"\x1b[7m  /exit" in raw, raw
            repl.send("\r")
            repl.read_until("bye")
            assert repl.proc.wait(timeout=5) == 0
        finally:
            repl.close()
    print("repl-menu: live slash list and prefix filtering passed")


if __name__ == "__main__":
    main()
