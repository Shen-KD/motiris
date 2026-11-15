#!/usr/bin/env python3
"""Offline integration tests for the bounded web_fetch receive buffer."""

import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlparse

FETCH_CAP = 64 * 1024
ROOT = Path(__file__).resolve().parent.parent


class WebHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        if parsed.path == "/html":
            body = (b"<html><body><h1>Hi there</h1>"
                    b" web ok</body></html>")
            chunk = len(body)
        elif parsed.path == "/bytes":
            length = int(query.get("n", ["0"])[0])
            chunk = int(query.get("chunk", ["16384"])[0])
            if length < 0 or length > 4 * 1024 * 1024 or chunk < 1:
                self.send_error(400)
                return
            body = b"A" * length
        else:
            self.send_error(404)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        for offset in range(0, len(body), chunk):
            try:
                self.wfile.write(body[offset:offset + chunk])
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                break

    def log_message(self, *args):
        pass


class ProviderHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        messages = request.get("messages", [])
        if messages and messages[-1].get("role") == "tool":
            # Return the actual tool JSON unchanged so the test can inspect the
            # complete bounded result rather than a provider-side summary.
            content = messages[-1].get("content", "")
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": content}}]}
        else:
            call = {"id": "call_web", "type": "function", "function": {
                "name": "web_fetch",
                "arguments": json.dumps({"url": self.server.target_url})}}
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": [call]}}]}
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def start_server(handler):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def run_fetch(binary, env, provider, path, query=None):
    query = query or {}
    suffix = urlencode(query)
    provider.target_url = "http://127.0.0.1:{}{}{}".format(
        provider.web_port, path, "?" + suffix if suffix else "")
    proc = subprocess.run(
        [str(binary), "-p", "fetch", "--transport", "libcurl",
         "--max-steps", "3", "-k", "x"],
        cwd=str(ROOT), env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=20)
    if proc.returncode != 0:
        raise AssertionError("motiris rc={}\nstdout={}\nstderr={}".format(
            proc.returncode, proc.stdout, proc.stderr))
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError("invalid final JSON: {}\nstdout={}\nstderr={}".format(
            exc, proc.stdout, proc.stderr))


def main():
    binary = ROOT / "motiris"
    if not binary.is_file():
        raise AssertionError("build motiris before running webtools tests")

    web, web_thread = start_server(WebHandler)
    provider, provider_thread = start_server(ProviderHandler)
    provider.web_port = web.server_address[1]
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-webtools-") as tmp:
            tmp = Path(tmp)
            home = tmp / "home"
            config = tmp / "config"
            plugins = tmp / "plugins"
            schemas = tmp / "schemas"
            for directory in (home, config, plugins, schemas):
                directory.mkdir()
            (config / "config.json").write_text(json.dumps({
                "transport": "libcurl",
                "base_url": "http://127.0.0.1:{}/v1/chat/completions".format(
                    provider.server_address[1])}))
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("MOTIRIS_")}
            env.update({"HOME": str(home), "MOTIRIS_HOME": str(config),
                        "MOTIRIS_PLUGIN_DIR": str(plugins),
                        "MOTIRIS_TOOLS_DIR": str(schemas),
                        "NO_PROXY": "127.0.0.1,localhost",
                        "no_proxy": "127.0.0.1,localhost"})

            result = run_fetch(binary, env, provider, "/html")
            assert result.get("text") == "Hi there web ok", result
            assert result.get("truncated", False) is False, result
            print("webtools: small HTML ok", flush=True)

            # Exercise exact allocation boundaries, byte segmentation and both
            # sides of the 64 KiB cap through a real model tool-call and GET.
            cases = [
                (16383, 16384),
                (16384, 16384),
                (16385, 16384),
                (16384, 1),
                (32768, 7),
                (65535, 4096),
                (65536, 16384),
                (65537, 8192),
                (131072, 32768),
                # The server may keep sending after the cap; motiris must drain
                # and discard without growing its receive buffer or failing GET.
                (1048576, 65536),
                (0, 16384),
            ]
            for length, chunk in cases:
                result = run_fetch(binary, env, provider, "/bytes",
                                   {"n": length, "chunk": chunk})
                expected_length = min(length, FETCH_CAP)
                expected_text = "A" * expected_length
                assert result.get("text") == expected_text, (
                    length, chunk, len(result.get("text", "")),
                    expected_length, result.get("error"))
                assert result.get("truncated", False) is (length > FETCH_CAP), (
                    length, chunk, result.get("truncated"))
                print("webtools: n={} chunk={} kept={} truncated={} ok".format(
                    length, chunk, len(result["text"]),
                    result.get("truncated", False)), flush=True)

            usage = resource.getrusage(resource.RUSAGE_CHILDREN)
            if sys.platform.startswith("linux"):
                print("webtools: max child RSS across cases: {} KiB".format(
                    usage.ru_maxrss), flush=True)
    finally:
        for server, thread in ((provider, provider_thread), (web, web_thread)):
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    main()
