#!/usr/bin/env python3
"""Offline deadline, output-bound and cleanup tests for browser/subagent."""

import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "motiris"
CAPS = {"browser_fetch": 64 * 1024, "subagent": 32 * 1024}
TIMEOUT_ENV = {
    "browser_fetch": "MOTIRIS_BROWSER_TIMEOUT_S",
    "subagent": "MOTIRIS_SUBAGENT_TIMEOUT_S",
}

FAKE_CHILD = r'''#!/usr/bin/env python3
import json, os, signal, subprocess, sys, time
log_path = os.environ["FAKE_CHILD_LOG"]
is_browser = len(sys.argv) > 1 and sys.argv[1] == "--headless"
mode = sys.argv[-1] if is_browser else sys.argv[2]
valid = {"normal", "exit7", "flood", "silent", "ignore", "orphan",
         "background", "continuous"}
if mode not in valid:
    mode = "normal"

def parent_stats():
    ppid = os.getppid()
    try:
        fds = len(os.listdir("/proc/%d/fd" % ppid))
        rss = 0
        with open("/proc/%d/status" % ppid) as status:
            for row in status:
                if row.startswith("VmRSS:"):
                    rss = int(row.split()[1])
        return fds, rss
    except (FileNotFoundError, ProcessLookupError):
        return -1, -1

def log(role, pid=None):
    fds, rss = parent_stats()
    with open(log_path, "a") as stream:
        stream.write(json.dumps({"role": role, "mode": mode,
            "pid": os.getpid() if pid is None else pid,
            "parent_fds": fds, "parent_rss": rss}) + "\n")

def output(text):
    sys.stdout.write(text)
    sys.stdout.flush()

log("leader")
if mode == "normal":
    output("<html><body>browser normal</body></html>" if is_browser
           else "subagent normal")
    sys.exit(0)
if mode == "exit7":
    output("child failure")
    sys.exit(7)
if mode == "flood":
    chunk = b"x" * 4096
    for index in range(48):
        os.write(1 if index % 2 == 0 else 2, chunk)
    sys.exit(0)
if mode == "continuous":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    while True:
        os.write(1, b"y" * 4096)
if mode in ("silent", "ignore", "orphan", "background"):
    redirect = subprocess.DEVNULL if mode == "background" else None
    descendant = subprocess.Popen([sys.executable, "-c",
        "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
        "time.sleep(60)"], stdout=redirect, stderr=redirect)
    log("descendant", descendant.pid)
    if mode in ("orphan", "background"):
        output("leader exited")
        sys.exit(0)
    if mode == "ignore":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    output("waiting")
    time.sleep(60)
'''


def process_state(pid):
    try:
        text = Path("/proc/%d/stat" % pid).read_text()
    except FileNotFoundError:
        return None
    return text[text.rfind(")") + 2:].split()[0]


def process_rss_kib(pid):
    for row in Path("/proc/%d/status" % pid).read_text().splitlines():
        if row.startswith("VmRSS:"):
            return int(row.split()[1])
    raise AssertionError("VmRSS missing for pid %d" % pid)


class Provider:
    def __init__(self):
        self.requests = []
        self.calls = []
        self.results = []
        self.proc = None
        self.final_fds = -1
        self.final_children = "not checked"
        self.final_rss = -1
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                request = json.loads(self.rfile.read(length))
                owner.requests.append(request)
                messages = request.get("messages", [])
                tool_messages = [m for m in messages if m.get("role") == "tool"
                    and m.get("tool_call_id", "").startswith("child-")]
                if tool_messages:
                    owner.results = [json.loads(m["content"])
                                     for m in tool_messages]
                    if owner.proc is not None and sys.platform.startswith("linux"):
                        root = Path("/proc") / str(owner.proc.pid)
                        owner.final_fds = len(list((root / "fd").iterdir()))
                        children = root / "task" / str(owner.proc.pid) / "children"
                        owner.final_children = children.read_text().strip()
                        owner.final_rss = process_rss_kib(owner.proc.pid)
                    message = {"role": "assistant",
                               "content": "bounded child results received"}
                    finish = "stop"
                else:
                    calls = []
                    for index, call in enumerate(owner.calls):
                        calls.append({"id": "child-%d" % index, "type": "function",
                            "function": {"name": call["name"],
                                         "arguments": json.dumps(call["arguments"])}})
                    message = {"role": "assistant", "tool_calls": calls}
                    finish = "tool_calls"
                body = json.dumps({"choices": [{
                    "finish_reason": finish, "message": message}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d/v1/chat/completions" % self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class Harness:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix="motiris-childproc-"))
        self.fake = self.temp / "fake-child"
        self.fake.write_text(FAKE_CHILD)
        self.fake.chmod(0o755)
        self.provider = Provider()
        self.sequence = 0
        self.logs = []

    def close(self):
        self.provider.close()
        for log in self.logs:
            if log.exists():
                for record in read_records(log):
                    for target in (-record["pid"], record["pid"]):
                        try:
                            os.kill(target, signal.SIGKILL)
                        except OSError:
                            pass
        shutil.rmtree(self.temp)

    def run(self, tool, modes, timeout="1"):
        self.sequence += 1
        home = self.temp / ("home-%d" % self.sequence)
        home.mkdir()
        log = self.temp / ("children-%d.jsonl" % self.sequence)
        self.logs.append(log)
        (home / "config.json").write_text(json.dumps({
            "transport": "libcurl", "base_url": self.provider.url}))
        env = {k: v for k, v in os.environ.items() if not k.startswith("MOTIRIS_")}
        env.update({
            "HOME": str(home), "MOTIRIS_HOME": str(home),
            "MOTIRIS_PLUGIN_DIR": str(home / "plugins"),
            "MOTIRIS_TOOLS_DIR": str(home / "schemas"),
            "MOTIRIS_BROWSER": str(self.fake), "MOTIRIS_BIN": str(self.fake),
            "FAKE_CHILD_LOG": str(log), TIMEOUT_ENV[tool]: str(timeout),
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        })
        if tool == "browser_fetch":
            arguments = [{"url": mode} for mode in modes]
        else:
            arguments = [{"task": mode} for mode in modes]
        self.provider.calls = [{"name": tool, "arguments": arg} for arg in arguments]
        self.provider.results = []
        self.provider.requests = []
        self.provider.final_fds = -1
        self.provider.final_children = "not checked"
        self.provider.final_rss = -1
        proc = subprocess.Popen(
            [str(BIN), "-p", "run bounded child tools", "--max-steps", "3",
             "--no-plugin", "-k", "x"], cwd=ROOT, env=env, text=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, start_new_session=True)
        self.provider.proc = proc
        started = time.monotonic()
        try:
            stdout, stderr = proc.communicate(timeout=25)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate(timeout=5)
            raise AssertionError("motiris %s %s did not finish\n%s\n%s" %
                                 (tool, modes, stdout, stderr))
        elapsed = time.monotonic() - started
        assert proc.returncode == 0, (proc.returncode, stdout, stderr)
        assert "bounded child results received" in stdout, (stdout, stderr)
        assert len(self.provider.results) == len(modes), self.provider.requests
        records = read_records(log) if log.exists() else []
        assert_cleaned_up(records)
        return self.provider.results, records, elapsed


def read_records(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def assert_cleaned_up(records):
    deadline = time.monotonic() + 2
    pending = [r for r in records if r["role"] in ("leader", "descendant")]
    while pending and time.monotonic() < deadline:
        pending = [r for r in pending if process_state(r["pid"]) is not None
                   and not (r["role"] == "descendant" and process_state(r["pid"]) == "Z")]
        if pending:
            time.sleep(0.02)
    assert not pending, "child processes remain: %r" % pending


def assert_normal(tool, result):
    assert result["exit_code"] == 0, result
    assert result["signal"] == 0, result
    assert result["timed_out"] is False, result
    assert result["truncated"] is False, result
    assert "error" not in result, result
    key = "text" if tool == "browser_fetch" else "output"
    expected = "browser normal" if tool == "browser_fetch" else "subagent normal"
    assert result[key] == expected, result


def exercise_tool(harness, tool):
    results, _, _ = harness.run(tool, ["normal"])
    assert_normal(tool, results[0])
    print("%s: normal result and exit status ok" % tool, flush=True)

    results, _, _ = harness.run(tool, ["exit7"])
    result = results[0]
    assert result["exit_code"] == 7 and "status 7" in result["error"], result
    print("%s: nonzero exit is model-visible" % tool, flush=True)

    results, _, elapsed = harness.run(tool, ["flood"])
    result = results[0]
    key = "text" if tool == "browser_fetch" else "output"
    assert len(result[key]) == CAPS[tool], (len(result[key]), CAPS[tool])
    assert result["truncated"] is True and result["timed_out"] is False, result
    assert result["exit_code"] == 0 and elapsed < 8, (result, elapsed)
    print("%s: 192 KiB dual-stream flood drained at %d-byte cap" %
          (tool, CAPS[tool]), flush=True)

    results, records, elapsed = harness.run(tool, ["silent"])
    result = results[0]
    assert result["timed_out"] is True and "timed out" in result["error"], result
    assert result["exit_code"] == -1, result
    assert result["signal"] in (signal.SIGTERM, signal.SIGKILL), result
    assert 0.7 <= elapsed < 6, elapsed
    assert {r["role"] for r in records} == {"leader", "descendant"}, records
    print("%s: silent process group timed out and was cleaned in %.3fs" %
          (tool, elapsed), flush=True)

    results, _, elapsed = harness.run(tool, ["ignore"])
    result = results[0]
    assert result["timed_out"] is True and result["signal"] == signal.SIGKILL, result
    assert 1.7 <= elapsed < 7, elapsed
    print("%s: TERM-ignoring leader escalated to KILL in %.3fs" %
          (tool, elapsed), flush=True)

    results, records, elapsed = harness.run(tool, ["orphan"])
    result = results[0]
    assert result["timed_out"] is True and result["exit_code"] == 0, result
    assert 0.7 <= elapsed < 6, elapsed
    assert any(r["role"] == "descendant" for r in records), records
    print("%s: exited leader cannot leave a pipe-holding descendant" % tool,
          flush=True)

    results, records, elapsed = harness.run(tool, ["background"])
    result = results[0]
    assert result["exit_code"] == 0 and result["timed_out"] is False, result
    assert elapsed < 3, elapsed
    assert any(r["role"] == "descendant" for r in records), records
    print("%s: successful leader cannot leave a DEVNULL background child" % tool,
          flush=True)

    results, _, elapsed = harness.run(tool, ["continuous"])
    result = results[0]
    key = "text" if tool == "browser_fetch" else "output"
    assert result["timed_out"] is True and result["truncated"] is True, result
    assert len(result[key]) == CAPS[tool], (len(result[key]), CAPS[tool])
    assert 0.7 <= elapsed < 7, elapsed
    print("%s: continuous output cannot extend the deadline (%.3fs)" %
          (tool, elapsed), flush=True)

    # Three timeouts in one Agent expose fd/child leaks. The later healthy call
    # proves that a timeout does not poison subsequent tool execution.
    results, records, _ = harness.run(tool, ["silent", "silent", "silent", "normal"])
    assert [r["timed_out"] for r in results] == [True, True, True, False], results
    assert_normal(tool, results[-1])
    leaders = [r for r in records if r["role"] == "leader"]
    assert len(leaders) == 4, leaders
    silent_leaders = [r for r in leaders if r["mode"] == "silent"]
    assert len({r["parent_fds"] for r in silent_leaders}) == 1, silent_leaders
    steady_rss = [r["parent_rss"] for r in silent_leaders[1:]]
    assert max(steady_rss) - min(steady_rss) <= 1024, steady_rss
    assert 0 <= harness.provider.final_fds < 32, harness.provider.final_fds
    assert harness.provider.final_children == "", harness.provider.final_children
    print("%s: repeated timeouts stable fds=%r RSS=%r KiB; final Agent fds=%d "
          "RSS=%d KiB, no direct children" %
          (tool, [r["parent_fds"] for r in silent_leaders],
           [r["parent_rss"] for r in silent_leaders], harness.provider.final_fds,
           harness.provider.final_rss), flush=True)

    for invalid in ("0", "121"):
        results, records, elapsed = harness.run(tool, ["normal"], timeout=invalid)
        result = results[0]
        assert TIMEOUT_ENV[tool] in result["error"], result
        assert result["exit_code"] == -1 and not records, (result, records)
        assert elapsed < 3, elapsed
    print("%s: invalid timeout fails before fork" % tool, flush=True)


def main():
    assert BIN.is_file(), "build motiris before running browser/subagent tests"
    harness = Harness()
    try:
        exercise_tool(harness, "browser_fetch")
        exercise_tool(harness, "subagent")

        # Exec failure is a bounded status, not an indefinite wait or false success.
        for tool, variable in (("browser_fetch", "MOTIRIS_BROWSER"),
                               ("subagent", "MOTIRIS_BIN")):
            original = os.environ.get(variable)
            os.environ[variable] = str(harness.temp / "does-not-exist")
            # Harness removes MOTIRIS_* from the inherited environment, so pass
            # the missing path by temporarily replacing its fake executable path.
            fake = harness.fake
            harness.fake = Path(os.environ[variable])
            try:
                results, records, _ = harness.run(tool, ["normal"])
            finally:
                harness.fake = fake
                if original is None:
                    os.environ.pop(variable, None)
                else:
                    os.environ[variable] = original
            assert results[0]["exit_code"] == 127, results[0]
            assert "status 127" in results[0]["error"], results[0]
            assert not records, records
        print("browser/subagent: missing executables return bounded status 127", flush=True)
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        print("browser/subagent: all bounded child tests passed; test-driver "
              "max child RSS=%d KiB" % rss, flush=True)
    finally:
        harness.close()


if __name__ == "__main__":
    main()
