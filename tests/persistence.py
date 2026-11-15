#!/usr/bin/env python3
"""Offline integration tests for PERSISTENCE=0 (memory-only full CLI)."""
import fcntl
import http.client
import http.server
import json
import os
import pty
import select
import socket
import struct
import subprocess
import tempfile
import termios
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"
UNAVAILABLE = "snapshot persistence unavailable"


class Provider(http.server.BaseHTTPRequestHandler):
    captures = []

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.captures.append(request)
        body = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": "assistant-ok"}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def invoke(args, env, expected=0, input_text=None):
    result = subprocess.run([str(BINARY)] + args, env=env, text=True,
                            input=input_text, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if result.returncode != expected:
        raise AssertionError("rc=%d, expected=%d\nstdout=%s\nstderr=%s" %
                             (result.returncode, expected,
                              result.stdout, result.stderr))
    return result


def read_until(fd, token, timeout=10):
    data = b""
    cursor_queries = 0
    deadline = time.monotonic() + timeout
    while token not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                data += os.read(fd, 65536)
            except OSError:
                break
            while data.count(b"\x1b[6n") > cursor_queries:
                os.write(fd, b"\x1b[1;1R")
                cursor_queries += 1
    if token not in data:
        raise AssertionError("PTY did not produce %r: %r" % (token, data[-1000:]))
    return data


def test_repl_no_autosave(env):
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
    output = b""
    try:
        read_until(master, b"motiris>")
        os.write(master, b"pty user\r")
        output = read_until(master, b"echo round complete")
        if output.rfind(b"motiris>") < output.find(b"echo round complete"):
            output += read_until(master, b"motiris>")
        proc.terminate()
        proc.wait(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)
    if b"session checkpoint not saved" in output:
        raise AssertionError("cut REPL attempted a checkpoint: %r" % output)
    logs = list((home / ".local/share/motiris/sessions").glob("*.log"))
    if logs:
        raise AssertionError("cut REPL wrote checkpoints: %r" % logs)


def test_gateway(env):
    root = Path(env["HOME"])
    proc = None
    log = None
    port = None

    def start():
        nonlocal proc, log, port
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        log = open(root / "gateway.log", "ab", buffering=0)
        proc = subprocess.Popen(
            [str(BINARY), "--gateway", "127.0.0.1:%d" % port,
             "--no-tools", "--no-plugin"],
            env=env, stdout=log, stderr=subprocess.STDOUT)
        for _ in range(50):
            if proc.poll() is not None:
                raise AssertionError("gateway exited with %s" % proc.returncode)
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                conn.request("GET", "/health")
                response = conn.getresponse()
                response.read()
                conn.close()
                if response.status == 200:
                    return
            except OSError:
                time.sleep(0.1)
        raise AssertionError("gateway readiness timeout")

    def stop():
        nonlocal proc, log
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            proc = None
        if log is not None:
            log.close()
            log = None

    def chat(message, reset=False):
        payload = {"chat_id": "volatile", "message": message}
        if reset:
            payload["reset"] = True
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        conn.request("POST", "/v1/chat", json.dumps(payload),
                     {"Content-Type": "application/json"})
        response = conn.getresponse()
        raw = response.read()
        status = response.status
        conn.close()
        if status != 200:
            raise AssertionError("gateway HTTP %d: %s" % (status, raw[:300]))
        body = json.loads(raw)
        if body.get("checkpoint") != "volatile-only" or "checkpoint_error" in body:
            raise AssertionError("gateway did not report volatile-only: %r" % body)
        return body

    def users():
        return [m.get("content") for m in Provider.captures[-1]["messages"]
                if m.get("role") == "user"]

    try:
        start()
        chat("ONE")
        chat("TWO")
        if users() != ["ONE", "TWO"]:
            raise AssertionError("in-process volatile history missing: %r" % users())
        logdir = root / ".local/share/motiris/gateway"
        if logdir.exists():
            raise AssertionError("cut gateway created its snapshot directory")

        stop()
        start()
        chat("THREE")
        if users() != ["THREE"]:
            raise AssertionError("cut gateway restored state across restart: %r" % users())

        # Reset clears RAM but must not touch a caller-placed file in the cut build.
        logdir.mkdir(parents=True)
        sentinel = logdir / ("volatile".encode().hex() + ".log")
        sentinel.write_text("do-not-unlink")
        chat("FOUR", reset=True)
        if users() != ["FOUR"]:
            raise AssertionError("cut gateway reset retained RAM history: %r" % users())
        if sentinel.read_text() != "do-not-unlink":
            raise AssertionError("cut gateway reset modified a snapshot path")
    finally:
        stop()


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        home = root / "home"
        config = root / "config"
        home.mkdir()
        config.mkdir()
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("MOTIRIS_"):
                env.pop(key)
        env["HOME"] = str(home)
        env["MOTIRIS_HOME"] = str(config)

        provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        base_url = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_address[1]
        (config / "config.json").write_text(json.dumps({
            "transport": "libcurl", "base_url": base_url}))
        common = ["--transport", "libcurl", "-b", base_url, "-k", "x",
                  "--no-tools", "--no-plugin"]
        try:
            before = len(Provider.captures)
            save_path = root / "must-not-exist.log"
            saved = invoke(["-p", "one", "--save", str(save_path)] + common,
                           env, expected=1)
            if UNAVAILABLE not in saved.stderr or save_path.exists():
                raise AssertionError("explicit save did not fail closed: %s" % saved.stderr)
            if len(Provider.captures) != before + 1:
                raise AssertionError("successful turn did not reach the provider")

            before = len(Provider.captures)
            sentinel = root / "sentinel.log"
            sentinel.write_text("do-not-read-or-modify")
            resumed = invoke(["-r", str(sentinel), "-p", "must-not-send"] + common,
                             env, expected=2)
            if UNAVAILABLE not in resumed.stderr:
                raise AssertionError("explicit resume diagnostic: %s" % resumed.stderr)
            if sentinel.read_text() != "do-not-read-or-modify":
                raise AssertionError("cut resume modified its input path")
            if len(Provider.captures) != before:
                raise AssertionError("rejected resume reached the provider")

            repl = invoke(["-i"] + common, env,
                          input_text="/resume %s\n/exit\n" % sentinel)
            if UNAVAILABLE not in repl.stdout:
                raise AssertionError("REPL /resume diagnostic: %s%s" %
                                     (repl.stdout, repl.stderr))
            if len(Provider.captures) != before:
                raise AssertionError("REPL /resume reached the provider")

            test_repl_no_autosave(env)
            gateway_env = env.copy()
            gateway_home = root / "gateway-home"
            gateway_home.mkdir()
            gateway_env["HOME"] = str(gateway_home)
            test_gateway(gateway_env)
        finally:
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=5)
    print("persistence: explicit fail-closed stubs + volatile REPL/gateway passed")


if __name__ == "__main__":
    main()
