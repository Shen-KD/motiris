#!/usr/bin/env python3
"""REPL recovery from a truncated stream with lost async wakeups (#101)."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading

from repl_cards import Repl, environment, plain

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"
INTERPOSER = ROOT / "build" / "repl_drop_async_wake.so"


def compact(value):
    return json.dumps(value, separators=(",", ":")).encode()


def delta(text):
    return b"data: " + compact({"choices": [{"delta": {
        "content": text}}]}) + b"\n\n"


def complete(text):
    return delta(text) + b"data: [DONE]\n\n"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.condition = threading.Condition()
        self.requests = []
        self.release_first_response = False

    def wait_requests(self, count, timeout=5):
        with self.condition:
            return self.condition.wait_for(lambda: len(self.requests) >= count,
                                           timeout=timeout)

    def wait_release(self):
        with self.condition:
            self.condition.wait_for(lambda: self.release_first_response,
                                    timeout=15)

    def release_failure(self):
        with self.condition:
            self.release_first_response = True
            self.condition.notify_all()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.condition:
            self.server.requests.append(json.loads(raw))
            call = len(self.server.requests)
            self.server.condition.notify_all()

        if call == 1:
            body = delta("before-disconnect")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body) + 64))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.server.wait_release()
            self.close_connection = True
            return

        text = "after-disconnect" if call == 2 else "third-after-recovery"
        body = complete(text)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True


def users(request):
    return [message.get("content") for message in request["messages"]
            if message.get("role") == "user"]


def build_interposer():
    INTERPOSER.parent.mkdir(exist_ok=True)
    subprocess.run([
        os.environ.get("CC", "cc"), "-shared", "-fPIC", "-std=c11",
        "-Wall", "-Wextra", "-Werror",
        str(ROOT / "tests" / "repl_drop_async_wake.c"),
        "-ldl", "-o", str(INTERPOSER),
    ], cwd=ROOT, check=True, timeout=30)


def asan_runtime():
    linked = subprocess.run(["ldd", str(BIN)], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, check=True, timeout=10)
    match = re.search(r"libasan\.so[^\s]*\s+=>\s+(\S+)", linked.stdout)
    return match.group(1) if match else None


def main():
    build_interposer()
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    home = tempfile.TemporaryDirectory(prefix="motiris-stream-recovery-")
    repl = None
    try:
        root = Path(home.name)
        env = environment(root)
        # ASan requires its own runtime to precede every other preloaded DSO.
        asan = asan_runtime()
        env["LD_PRELOAD"] = ":".join(
            ([asan] if asan else []) + [str(INTERPOSER)])
        drop_count = root / "dropped-wakes"
        env["MOTIRIS_TEST_WAKE_DROP_FILE"] = str(drop_count)
        (root / "config" / "config.json").write_text(json.dumps({
            "transport": "libcurl",
            "stream": True,
            "max_retries": 0,
            "retry_base_seconds": 0,
            "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                        provider.server_port,
        }))
        repl = Repl(env, command=[
            str(BIN), "-i", "--no-tools", "--no-plugin", "--max-steps", "1",
        ])
        repl.read_until("motiris> ")
        repl.clear()
        repl.send("first instruction\r")
        assert provider.wait_requests(1), "first request did not start"

        # Queue input while the first response is paused. The interposer drops
        # every async wakeup, so only terminal input can currently wake main.
        repl.send("second instruction\r")
        repl.read_until("[queued 1]")
        provider.release_failure()

        # A bounded active-worker wait must drain the orphaned events, collect
        # the finished worker and automatically dispatch the queued request.
        recovered = plain(repl.read_until(re.compile(
            r"before-disconnect[\s\S]*after-disconnect[\s\S]*motiris> "),
            timeout=5))
        assert "motiris:" in recovered, recovered
        assert provider.wait_requests(2), "queued request was not dispatched"
        assert users(provider.requests[0]) == ["first instruction"]
        assert users(provider.requests[1]) == ["first instruction",
                                               "second instruction"]
        assert "before-disconnect" not in json.dumps(provider.requests[1])

        # Verify input submitted after the failure also works, not only input
        # that happened to be queued while the disconnect occurred.
        repl.clear()
        repl.send("third instruction\r")
        repl.read_until("third-after-recovery")
        assert provider.wait_requests(3), "post-recovery request did not start"
        assert users(provider.requests[2])[-1] == "third instruction"
        assert "[queued" not in plain(repl.output), plain(repl.output)
        repl.send("/exit\r")
        repl.read_until("bye")
        assert repl.proc.wait(timeout=5) == 0
        dropped = int(drop_count.read_text().strip())
        assert dropped >= 2, "fault injection did not drop async wakeups"
        print("repl-stream-recovery: truncated stream, %d dropped wakeups "
              "and FIFO recovery passed" % dropped)
    finally:
        provider.release_failure()
        if repl:
            repl.close()
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=2)
        home.cleanup()


if __name__ == "__main__":
    main()
