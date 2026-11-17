#!/usr/bin/env python3
"""Offline integration tests for bounded provider response handling."""

from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"
LIMIT_ERROR = "response limit exceeded"
MAX_DIAGNOSTIC_BYTES = 2048


def compact(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def chat_response(text):
    return compact({"choices": [{"finish_reason": "stop", "message": {
        "role": "assistant", "content": text}}]})


def delta(content=None, tool_calls=None):
    value = {}
    if content is not None:
        value["content"] = content
    if tool_calls is not None:
        value["tool_calls"] = tool_calls
    return {"choices": [{"delta": value}]}


def sse_response(events):
    return b"".join(b"data: " + compact(event) + b"\n\n"
                    for event in events) + b"data: [DONE]\n\n"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.responses = {}
        self.hits = Counter()

    def add(self, name, body, stream=False, chunk_size=None):
        path = "/v1/chat/completions/" + name
        content_type = "text/event-stream" if stream else "application/json"
        self.responses[path] = (body, content_type, chunk_size or len(body))
        return path


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        self.server.hits[self.path] += 1
        response = self.server.responses.get(self.path)
        if response is None:
            self.send_error(404)
            return
        body, content_type, chunk_size = response
        try:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            for offset in range(0, len(body), chunk_size):
                self.wfile.write(body[offset:offset + chunk_size])
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            # Limit enforcement intentionally aborts several responses early.
            pass
        self.close_connection = True

    def log_message(self, *_args):
        pass


def execute(command, env):
    proc = subprocess.Popen(
        command, cwd=str(ROOT), env=env, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace", start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=8)
    except subprocess.TimeoutExpired as exc:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = proc.communicate()
        raise AssertionError("command timed out: %r\nstdout=%s\nstderr=%s" %
                             (command, stdout, stderr)) from exc
    return subprocess.CompletedProcess(command, proc.returncode, stdout, stderr)


def details(proc):
    return "rc=%d\nstdout=%s\nstderr=%s" % (
        proc.returncode, proc.stdout, proc.stderr)


def expect_ok(proc, output):
    if proc.returncode != 0 or proc.stdout != output:
        raise AssertionError("expected output %r\n%s" % (output, details(proc)))


def expect_error(proc, message):
    if proc.returncode <= 0:
        raise AssertionError("failure did not exit cleanly nonzero\n%s" %
                             details(proc))
    if message not in proc.stderr:
        raise AssertionError("missing diagnostic %r\n%s" % (message, details(proc)))
    if len(proc.stderr.encode("utf-8")) > MAX_DIAGNOSTIC_BYTES:
        raise AssertionError("diagnostic was not bounded: %d bytes" %
                             len(proc.stderr.encode("utf-8")))


def expect_limit(proc):
    expect_error(proc, LIMIT_ERROR)


def invoke(provider, path, cap, base_env, transport="libcurl", stream=False):
    env = base_env.copy()
    env["MOTIRIS_RESPONSE_MAX_BYTES"] = str(cap)
    url = "http://127.0.0.1:%d%s" % (provider.server_port, path)
    command = [str(BINARY), "-p", "bounded response", "-b", url, "-k", "x",
               "--transport", transport, "--no-tools", "--no-plugin",
               "--max-steps", "1"]
    if stream:
        command.append("--stream")
    before = provider.hits[path]
    proc = execute(command, env)
    if provider.hits[path] != before + 1:
        raise AssertionError("provider request count changed unexpectedly: %s" %
                             details(proc))
    return proc


def main():
    if not BINARY.is_file():
        raise AssertionError("build motiris before running transport tests")

    provider = Provider()
    small_path = provider.add("small", chat_response("buffered ok"))
    large_path = provider.add("large", chat_response("B" * 600))

    valid_sse = sse_response([
        delta(content="hel"), delta(content="lo "), delta(content="world")])
    sse_path = provider.add("sse", valid_sse, stream=True, chunk_size=17)

    # Short comment lines overflow 4*cap total wire budget; content stays tiny.
    wire = (b": " + b"w" * 24 + b"\n\n") * 12
    wire += sse_response([delta(content="wire")])
    wire_path = provider.add("wire", wire, stream=True, chunk_size=13)

    # Every content event is a short line, but their aggregate exceeds the cap.
    aggregate = sse_response([delta(content="a" * 64) for _ in range(10)])
    aggregate_path = provider.add("aggregate", aggregate, stream=True,
                                  chunk_size=29)

    # Leading padding keeps the decoded content small while making one line huge.
    long_line = (b"data:" + b" " * 600 +
                 compact(delta(content="short")) + b"\n\n" + b"data: [DONE]\n\n")
    line_path = provider.add("line", long_line, stream=True, chunk_size=23)

    # Individual argument deltas are small; their merged argument string is not.
    tool_events = [delta(tool_calls=[{
        "index": 0, "id": "call_bounded", "type": "function",
        "function": {"name": "not_registered",
                     "arguments": "{\"command\":\""}}])]
    tool_events.extend(delta(tool_calls=[{
        "index": 0, "function": {"arguments": "t" * 80}}])
        for _ in range(8))
    tool_events.append(delta(tool_calls=[{
        "index": 0, "function": {"arguments": "\"}"}}]))
    tools_path = provider.add("tools", sse_response(tool_events), stream=True,
                              chunk_size=31)

    invalid_index_paths = []
    for label, index in (("missing", None), ("negative", -1),
                         ("fractional", 0.5), ("huge", 1e300)):
        call = {"id": "call_bad_index", "type": "function",
                "function": {"name": "not_registered", "arguments": "{}"}}
        if index is not None:
            call["index"] = index
        path = provider.add("index-" + label,
                            sse_response([delta(tool_calls=[call])]), stream=True)
        invalid_index_paths.append(path)

    spawned_path = provider.add("spawned", chat_response("C" * 900))
    exact = chat_response("exact boundary")
    exact += b" " * (256 - len(exact))
    exact_path = provider.add("exact", exact)

    # The listening socket is bound by Provider() before any child is launched.
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-transport-") as tmp:
            root = Path(tmp)
            home = root / "home"
            config = root / "config"
            home.mkdir()
            config.mkdir()
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("MOTIRIS_")}
            env.update({
                "HOME": str(home), "MOTIRIS_HOME": str(config),
                "NO_PROXY": "127.0.0.1,localhost",
                "no_proxy": "127.0.0.1,localhost",
            })

            expect_ok(invoke(provider, small_path, 1024, env), "buffered ok\n")
            expect_limit(invoke(provider, large_path, 256, env))
            expect_ok(invoke(provider, sse_path, 2048, env, stream=True),
                      "hello world\n")
            expect_limit(invoke(provider, wire_path, 64, env, stream=True))
            expect_limit(invoke(provider, aggregate_path, 512, env, stream=True))
            expect_limit(invoke(provider, line_path, 512, env, stream=True))
            expect_limit(invoke(provider, tools_path, 512, env, stream=True))
            for path in invalid_index_paths:
                expect_error(invoke(provider, path, 2048, env, stream=True),
                             "invalid stream response")
            expect_limit(invoke(provider, spawned_path, 512, env,
                                transport="curl"))
            expect_ok(invoke(provider, exact_path, len(exact), env),
                      "exact boundary\n")

            # The pre-existing offline transport remains usable without a server.
            echo = execute([str(BINARY), "-p", "hi", "--transport", "echo",
                            "--no-plugin", "--max-steps", "2"], env)
            expect_ok(echo, "echo round complete: [ok]\n")
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)

    print("transport: buffered, SSE, spawned-curl, exact-boundary and echo checks passed")


if __name__ == "__main__":
    main()
