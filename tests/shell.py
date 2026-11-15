#!/usr/bin/env python3
"""Offline integration tests for bounded host shell-tool execution."""

import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_STREAM_CAP = 16 * 1024
DEFAULT_TOTAL_CAP = 32 * 1024
HARD_TOTAL_CAP = 256 * 1024


def process_rss_kib(pid):
    status = Path("/proc/%d/status" % pid).read_text()
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise AssertionError("VmRSS missing for pid %d" % pid)


class ProviderHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        self.server.requests.append(request)
        messages = request.get("messages", [])
        tool_messages = [m for m in messages
                         if m.get("role") == "tool" and
                         m.get("tool_call_id", "").startswith("shell-")]
        if self.server.sample_rss and sys.platform.startswith("linux"):
            # Popen returns before the provider thread normally runs. The
            # bounded wait also closes that startup race on a loaded host.
            for _ in range(100):
                if self.server.proc is not None:
                    break
                time.sleep(0.01)
            rss = process_rss_kib(self.server.proc.pid)
            if tool_messages:
                self.server.rss_after = rss
            else:
                self.server.rss_before = rss
        if tool_messages:
            self.server.raw_results = [m.get("content", "")
                                       for m in tool_messages]
            self.server.results = [json.loads(content)
                                   for content in self.server.raw_results]
            if (self.server.check_leaks and self.server.proc is not None
                    and sys.platform.startswith("linux")):
                proc_root = Path("/proc") / str(self.server.proc.pid)
                self.server.fd_count = len(list((proc_root / "fd").iterdir()))
                children = proc_root / "task" / str(self.server.proc.pid) / "children"
                self.server.children = children.read_text().strip()
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": "shell result received"}}]}
        else:
            calls = []
            for index, command in enumerate(self.server.commands):
                calls.append({"id": "shell-%d" % index, "type": "function",
                              "function": {"name": "shell", "arguments":
                                           json.dumps({"command": command})}})
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": calls}}]}
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def process_running(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A reparented descendant can remain a zombie briefly. It is no longer
    # running and cannot hold a pipe or consume CPU.
    try:
        stat = Path("/proc/%d/stat" % pid).read_text()
        if stat.rsplit(")", 1)[1].split()[0] == "Z":
            return False
    except FileNotFoundError:
        if sys.platform.startswith("linux"):
            return False
    except IndexError:
        pass
    return True


def main():
    binary = ROOT / "motiris"
    if not binary.is_file():
        raise AssertionError("build motiris before running shell tests")

    provider = ThreadingHTTPServer(("127.0.0.1", 0), ProviderHandler)
    provider.daemon_threads = True
    provider.requests = []
    provider.commands = []
    provider.results = []
    provider.raw_results = []
    provider.check_leaks = False
    provider.sample_rss = False
    provider.rss_before = -1
    provider.rss_after = -1
    provider.proc = None
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()

    try:
        with tempfile.TemporaryDirectory(prefix="motiris-shell-") as tmp:
            tmp = Path(tmp)
            home = tmp / "home"
            config = tmp / "config"
            plugins = tmp / "plugins"
            schemas = tmp / "schemas"
            for directory in (home, config, plugins, schemas):
                directory.mkdir()
            (config / "config.json").write_text(json.dumps({
                "transport": "libcurl",
                "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                            provider.server_address[1],
            }))
            base_env = {key: value for key, value in os.environ.items()
                        if not key.startswith("MOTIRIS_")}
            base_env.update({"HOME": str(home), "MOTIRIS_HOME": str(config),
                             "MOTIRIS_PLUGIN_DIR": str(plugins),
                             "MOTIRIS_TOOLS_DIR": str(schemas),
                             "NO_PROXY": "127.0.0.1,localhost",
                             "no_proxy": "127.0.0.1,localhost"})

            def run_case(commands, limits=None, check_leaks=False,
                         sample_rss=False):
                if isinstance(commands, str):
                    commands = [commands]
                provider.commands = commands
                provider.results = []
                provider.raw_results = []
                provider.check_leaks = check_leaks
                provider.sample_rss = sample_rss
                provider.rss_before = -1
                provider.rss_after = -1
                provider.proc = None
                provider.fd_count = -1
                provider.children = "not checked"
                env = base_env.copy()
                if limits:
                    env.update({key: str(value) for key, value in limits.items()})
                proc = subprocess.Popen(
                    [str(binary), "-p", "run shell", "--max-steps", "3",
                     "--no-plugin", "-k", "x"], cwd=str(ROOT), env=env,
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    start_new_session=True)
                provider.proc = proc
                started = time.monotonic()
                try:
                    stdout, stderr = proc.communicate(timeout=20)
                except subprocess.TimeoutExpired:
                    # Also cleans up the pre-fix sequential-pipe reproduction.
                    os.killpg(proc.pid, signal.SIGKILL)
                    stdout, stderr = proc.communicate(timeout=5)
                    raise AssertionError(
                        "motiris shell case timed out\nstdout=%s\nstderr=%s" %
                        (stdout, stderr))
                elapsed = time.monotonic() - started
                assert proc.returncode == 0, (proc.returncode, stdout, stderr)
                assert "shell result received" in stdout, (stdout, stderr)
                assert len(provider.results) == len(commands), (
                    provider.results, provider.requests)
                return provider.results, provider.raw_results, elapsed

            results, _, _ = run_case(
                "printf out; printf err >&2; exit 7")
            result = results[0]
            assert result["stdout"] == "out", result
            assert result["stderr"] == "err", result
            assert result["exit_code"] == 7, result
            assert result["signal"] == 0, result
            assert result["timed_out"] is False, result
            assert result["truncated"] is False, result
            print("shell: short command + exit status ok", flush=True)

            results, _, _ = run_case("kill -TERM $$")
            result = results[0]
            assert result["exit_code"] == -1, result
            assert result["signal"] == signal.SIGTERM, result
            assert result["timed_out"] is False, result
            print("shell: non-timeout signal status ok", flush=True)

            # Regression from issue #42: stderr fills its pipe before stdout
            # reaches EOF. The old parent blocked forever reading stdout first.
            results, _, _ = run_case(
                "head -c 131072 /dev/zero | tr '\\0' 'e' >&2; printf done")
            result = results[0]
            assert result["stdout"] == "done", result
            assert len(result["stderr"]) == DEFAULT_STREAM_CAP, result
            assert result["stderr_truncated"] is True, result
            assert result["truncated"] is True, result
            assert result["exit_code"] == 0, result
            print("shell: 128 KiB stderr-first reproduction bounded", flush=True)

            line = "0123456789abcdef" * 4
            alternate = ("i=0; while [ \"$i\" -lt 1024 ]; do "
                         "printf '" + line + "'; printf '" + line + "' >&2; "
                         "i=$((i + 1)); done")
            results, _, _ = run_case(alternate)
            result = results[0]
            assert len(result["stdout"]) == DEFAULT_STREAM_CAP, result
            assert len(result["stderr"]) == DEFAULT_STREAM_CAP, result
            assert len(result["stdout"]) + len(result["stderr"]) == DEFAULT_TOTAL_CAP
            assert result["stdout_truncated"] is True, result
            assert result["stderr_truncated"] is True, result
            assert result["exit_code"] == 0, result
            print("shell: alternating dual-stream flood drained", flush=True)

            make_bytes = "head -c %d /dev/zero | tr '\\0' 'x'"
            results, _, _ = run_case(make_bytes % 64, {
                "MOTIRIS_SHELL_STDOUT_MAX": 64,
                "MOTIRIS_SHELL_STDERR_MAX": 64,
                "MOTIRIS_SHELL_OUTPUT_MAX": 64,
            })
            result = results[0]
            assert len(result["stdout"]) == 64, result
            assert result["truncated"] is False, result

            results, _, _ = run_case(make_bytes % 128, {
                "MOTIRIS_SHELL_STDOUT_MAX": 64,
                "MOTIRIS_SHELL_STDERR_MAX": 64,
                "MOTIRIS_SHELL_OUTPUT_MAX": 32,
            })
            result = results[0]
            assert len(result["stdout"]) == 32, result
            assert result["stdout_truncated"] is True, result
            assert result["truncated"] is True, result
            assert result["exit_code"] == 0, result
            print("shell: exact/total output cap boundaries ok", flush=True)

            marker = tmp / "sleep.pid"
            command = ("printf before; sleep 30 & echo $! > %s; wait" %
                       shlex.quote(str(marker)))
            results, _, elapsed = run_case(command, {
                "MOTIRIS_SHELL_TIMEOUT_S": 1,
            })
            result = results[0]
            assert result["timed_out"] is True, result
            assert result["stdout"] == "before", result
            assert result["exit_code"] == -1, result
            assert result["signal"] == signal.SIGKILL, result
            assert "timed out" in result["error"], result
            assert 0.5 <= elapsed < 5, elapsed
            sleeper = int(marker.read_text().strip())
            deadline = time.monotonic() + 2
            while process_running(sleeper) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not process_running(sleeper), "orphan sleep pid %d" % sleeper
            print("shell: timeout killed/reaped process group in %.3fs" %
                  elapsed, flush=True)

            results, _, _ = run_case("printf should-not-run", {
                "MOTIRIS_SHELL_TIMEOUT_S": 0,
            })
            result = results[0]
            assert "MOTIRIS_SHELL_TIMEOUT_S" in result["error"], result
            assert result["stdout"] == "", result
            assert result["exit_code"] == -1, result
            print("shell: invalid limits fail closed before fork", flush=True)

            # Repeated calls share one agent/process. Leaking four pipe fds per
            # call or one direct child would be visible by the second request.
            results, _, _ = run_case(["printf call-%d" % i for i in range(16)],
                                     check_leaks=True)
            assert [r["stdout"] for r in results] == [
                "call-%d" % i for i in range(16)]
            if sys.platform.startswith("linux"):
                assert 0 <= provider.fd_count < 24, provider.fd_count
                assert provider.children == "", provider.children
            print("shell: 16 repeated calls leave no fds/direct children", flush=True)

            # Maximum supported retained-output budget, through the provider.
            max_limits = {
                "MOTIRIS_SHELL_STDOUT_MAX": HARD_TOTAL_CAP,
                "MOTIRIS_SHELL_STDERR_MAX": HARD_TOTAL_CAP,
                "MOTIRIS_SHELL_OUTPUT_MAX": HARD_TOTAL_CAP,
            }
            results, raw_results, _ = run_case(make_bytes % 300000, max_limits,
                                               sample_rss=True)
            result = results[0]
            assert len(result["stdout"]) == HARD_TOTAL_CAP, result
            assert result["truncated"] is True, result
            assert len(raw_results[0]) < HARD_TOTAL_CAP + 4096
            usage = resource.getrusage(resource.RUSAGE_CHILDREN)
            if sys.platform.startswith("linux"):
                assert provider.rss_before > 0 and provider.rss_after > 0
                retained_delta = provider.rss_after - provider.rss_before
                # ASAN redzones inflate retained allocator pages; the host
                # assertion is a regression guard, not an MCU RAM budget.
                assert retained_delta < 32 * 1024, retained_delta
                print("shell: max-cap result=%d bytes, max child RSS=%d KiB, "
                      "agent RSS before/after=%d/%d KiB (delta=%d KiB)" %
                      (len(raw_results[0]), usage.ru_maxrss,
                       provider.rss_before, provider.rss_after,
                       retained_delta), flush=True)
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)

    print("shell: all bounded execution tests passed", flush=True)


if __name__ == "__main__":
    main()
