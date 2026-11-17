#!/usr/bin/env python3
"""Offline Gateway I/O deadline regressions; no external network or API key."""

import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

BINARY = Path(os.environ.get("ROOT", Path(__file__).resolve().parent.parent)) / "motiris"
PARTIAL = b"POST /v1/chat HTTP/1.1\r\nHost: localhost"


def receive_all(sock, timeout=4):
    sock.settimeout(timeout)
    data = b""
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            break
        if not chunk:
            break
        data += chunk
    return data


def health(port, timeout=3):
    # Reading through EOF waits for the server-side close, making subsequent
    # fd-count assertions independent of response-body scheduling.
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
        raw = receive_all(sock, timeout=timeout)
    headers, separator, body = raw.partition(b"\r\n\r\n")
    assert separator and headers.startswith(b"HTTP/1.1 200 "), raw[:300]
    assert json.loads(body)["status"] == "ok"


def clean_env(home, config_home, extra=None):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({"HOME": str(home), "MOTIRIS_HOME": str(config_home)})
    if extra:
        env.update(extra)
    return env


class Gateway:
    def __init__(self, root, config, extra_env=None):
        self.root = Path(root)
        self.home = self.root / "home"
        self.home.mkdir(parents=True)
        (self.root / "config.json").write_text(json.dumps(config))
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.log = open(self.root / "gateway.log", "wb", buffering=0)
        self.proc = subprocess.Popen(
            [str(BINARY), "--gateway", "127.0.0.1:%d" % self.port, "--no-tools"],
            env=clean_env(self.home, self.root, extra_env),
            stdout=self.log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if self.proc.poll() is not None:
                    raise AssertionError("gateway exited with %s: %s" %
                                         (self.proc.returncode, self.log_text()))
                try:
                    health(self.port, timeout=0.2)
                    return
                except (OSError, AssertionError):
                    time.sleep(0.05)
            raise AssertionError("gateway readiness timeout: " + self.log_text())
        except BaseException:
            self.stop()
            raise

    def log_text(self):
        return (self.root / "gateway.log").read_text(errors="replace")

    def fd_count(self):
        try:
            return len(list(Path("/proc/%d/fd" % self.proc.pid).iterdir()))
        except OSError:
            return None

    def rss_kib(self):
        try:
            status = Path("/proc/%d/status" % self.proc.pid).read_text()
            return int(next(line.split()[1] for line in status.splitlines()
                            if line.startswith("VmRSS:")))
        except (OSError, StopIteration):
            return None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()


def expect_read_timeout(port, request):
    started = time.monotonic()
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.sendall(request)
        raw = receive_all(sock)
    elapsed = time.monotonic() - started
    assert raw.startswith(b"HTTP/1.1 408 "), raw[:300]
    assert b"request timeout" in raw, raw[:300]
    assert 0.5 <= elapsed < 3.5, elapsed


def run_read_tests(root):
    gateway = Gateway(root / "read", {"transport": "echo", "tools": False},
                      {"MOTIRIS_GATEWAY_IO_TIMEOUT_S": "1"})
    try:
        fds_before = gateway.fd_count()
        rss_before = gateway.rss_kib()
        expect_read_timeout(gateway.port, PARTIAL)
        health(gateway.port)

        body = b'{"chat_id":"slow-body","message":"hi"}'
        partial_body = (b"POST /v1/chat HTTP/1.1\r\nContent-Length: " +
                        str(len(body)).encode() + b"\r\n\r\n" + body[:5])
        expect_read_timeout(gateway.port, partial_body)
        health(gateway.port)

        # Traffic must not extend the absolute receive deadline.
        with socket.create_connection(("127.0.0.1", gateway.port), timeout=2) as sock:
            sock.sendall(b"GET /health HTTP/1.1\r\nX-Slow: ")
            stopped = threading.Event()

            def trickle():
                while not stopped.is_set():
                    try:
                        sock.sendall(b"x")
                    except OSError:
                        return
                    time.sleep(0.15)

            sender = threading.Thread(target=trickle)
            sender.start()
            started = time.monotonic()
            try:
                raw = receive_all(sock)
            finally:
                stopped.set()
                sender.join(timeout=2)
            elapsed = time.monotonic() - started
            assert raw.startswith(b"HTTP/1.1 408 "), raw[:300]
            assert elapsed < 3.5, elapsed
        health(gateway.port)

        # Repeated expiry closes every accepted descriptor and does not grow RSS
        # through retained requests or a queue.
        for _ in range(2):
            expect_read_timeout(gateway.port, PARTIAL)
            health(gateway.port)
        fds_after = gateway.fd_count()
        rss_after = gateway.rss_kib()
        if fds_before is not None:
            assert fds_after == fds_before, (fds_before, fds_after)
        print("gateway I/O: repeated receive timeouts RSS %s -> %s KiB; fd %s -> %s" %
              (rss_before, rss_after, fds_before, fds_after))
    finally:
        gateway.stop()


class LargeProvider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        body = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": "x" * (12 * 1024 * 1024)}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run_write_test(root):
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), LargeProvider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    gateway = None
    slow = None
    try:
        config = {
            "transport": "libcurl",
            "base_url": "http://127.0.0.1:%d/v1/chat/completions" % provider.server_address[1],
        }
        gateway = Gateway(root / "write", config,
                          {"MOTIRIS_GATEWAY_IO_TIMEOUT_S": "1"})
        slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        slow.settimeout(2)
        slow.connect(("127.0.0.1", gateway.port))
        body = b'{"chat_id":"slow-reader","message":"hi"}'
        slow.sendall(b"POST /v1/chat HTTP/1.1\r\nContent-Length: " +
                     str(len(body)).encode() + b"\r\n\r\n" + body)

        # Do not read the large response. The single-threaded server must finish
        # its bounded nonblocking write and accept a healthy connection anyway.
        time.sleep(1.5)
        health(gateway.port)
        assert "response write timed out" in gateway.log_text(), gateway.log_text()
        print("gateway I/O: non-reading client response deadline passed")
    finally:
        if slow is not None:
            slow.close()
        if gateway is not None:
            gateway.stop()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)


def run_invalid_setting_test(root):
    config_home = root / "invalid"
    home = config_home / "home"
    home.mkdir(parents=True)
    (config_home / "config.json").write_text('{"transport":"echo","tools":false}')
    proc = subprocess.Popen(
        [str(BINARY), "--gateway", "127.0.0.1:0", "--no-tools"],
        env=clean_env(home, config_home, {"MOTIRIS_GATEWAY_IO_TIMEOUT_S": "0"}),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate(timeout=3)
        raise AssertionError("invalid I/O timeout did not fail before listen: " + output)
    assert proc.returncode != 0, output
    assert "MOTIRIS_GATEWAY_IO_TIMEOUT_S must be 1..120" in output, output


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-gateway-io-") as tmp:
        root = Path(tmp)
        root.mkdir(exist_ok=True)
        run_read_tests(root)
        run_write_test(root)
        run_invalid_setting_test(root)
    print("gateway I/O: receive/send monotonic deadlines and cleanup passed")


if __name__ == "__main__":
    main()
