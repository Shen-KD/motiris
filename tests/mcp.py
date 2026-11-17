#!/usr/bin/env python3
"""Offline MCP deadline, cleanup and failure-path integration tests."""
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

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"

SERVER_SOURCE = r'''
import json, os, signal, subprocess, sys, time
mode, pidfile = sys.argv[1:3]
parent_fds = len(os.listdir("/proc/%d/fd" % os.getppid()))
parent_rss = 0
with open("/proc/%d/status" % os.getppid()) as status:
    for row in status:
        if row.startswith("VmRSS:"):
            parent_rss = int(row.split()[1])
with open(pidfile, "w") as f:
    f.write("%d %d %d" % (os.getpid(), parent_fds, parent_rss))
if mode in ("empty-ignore-term", "orphan-after-init"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
if mode == "orphan-after-init":
    descendant = subprocess.Popen([sys.executable, "-c",
        "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
        "time.sleep(60)"])
    with open(pidfile + ".child", "w") as f:
        f.write(str(descendant.pid))

def reply(value):
    sys.stdout.write(json.dumps(value) + "\n")
    sys.stdout.flush()

for line in sys.stdin:
    try:
        req = json.loads(line)
    except Exception:
        continue
    method = req.get("method")
    rid = req.get("id")
    if method == "initialize":
        if mode == "silent-init":
            continue
        if mode == "oversize-init":
            sys.stdout.write(" " * (1048576 + 1))
            sys.stdout.flush()
            continue
        if mode == "sigpipe-after-init":
            # The response is sent only after stdin is closed, so the next
            # initialized-notification write deterministically raises EPIPE.
            os.close(0)
        reply({"jsonrpc":"2.0", "id":rid,
               "result":{"serverInfo":{"name":"fake","version":"1"}}})
        if mode in ("exit-after-init", "orphan-after-init"):
            sys.exit(0)
        if mode == "sigpipe-after-init":
            time.sleep(60)
    elif method == "tools/list":
        if mode == "silent-list":
            continue
        tools = [] if mode == "empty-ignore-term" else [{
            "name":"greet", "description":"hi", "inputSchema":{"type":"object"}
        }]
        reply({"jsonrpc":"2.0", "id":rid, "result":{"tools":tools}})
        if mode == "stall-call-write":
            # Leave the next, deliberately large request unread in the pipe.
            time.sleep(60)
    elif method == "tools/call":
        if mode == "silent-call":
            continue
        args = req.get("params", {}).get("arguments", {}) or {}
        reply({"jsonrpc":"2.0", "id":rid, "result":{"content":[{
            "type":"text", "text":"hello, " + str(args.get("who", "world")) + "!"
        }]}})

# Stay alive after stdin EOF as well as ignoring SIGTERM, forcing close's KILL path.
if mode == "empty-ignore-term":
    while True:
        time.sleep(1)
'''


def child_state(pid):
    """Return None when the grandchild is gone, or its Linux process state."""
    stat = Path(f"/proc/{pid}/stat")
    try:
        text = stat.read_text()
    except FileNotFoundError:
        return None
    # The comm field is parenthesized and may itself contain spaces.
    return text[text.rfind(")") + 2:].split()[0]


def kill_from_pidfile(pidfile):
    pid = int(pidfile.read_text().split()[0])
    for target in (-pid, pid):
        try:
            os.kill(target, signal.SIGKILL)
        except OSError:
            pass
    descendant = Path(str(pidfile) + ".child")
    if descendant.exists():
        try:
            os.kill(int(descendant.read_text()), signal.SIGKILL)
        except OSError:
            pass


def assert_child_gone(pidfile, allow_zombie=False):
    fields = pidfile.read_text().split()
    pid = int(fields[0])
    parent_fds = int(fields[1]) if len(fields) > 1 else -1
    parent_rss = int(fields[2]) if len(fields) > 2 else -1
    deadline = time.monotonic() + 2
    while child_state(pid) is not None and time.monotonic() < deadline:
        time.sleep(0.02)
    state = child_state(pid)
    if state is not None and not (allow_zombie and state == "Z"):
        kill_from_pidfile(pidfile)
        raise AssertionError(f"MCP child {pid} was not reaped (state={state})")
    descendant = Path(str(pidfile) + ".child")
    if descendant.exists():
        assert_child_gone(descendant, allow_zombie=True)
    return parent_fds, parent_rss


def maximum_write_argument():
    request = {"jsonrpc":"2.0", "id":3, "method":"tools/call",
               "params":{"name":"greet", "arguments":{"who":""}}}
    overhead = len(json.dumps(request, separators=(",", ":")).encode())
    # Exactly MAX_READ bytes plus the newline cannot fit even a 1 MiB pipe.
    return "x" * ((1 << 20) - overhead)


class Provider:
    def __init__(self, argument="motiris"):
        self.requests = []
        self.argument = argument
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                request = json.loads(self.rfile.read(length))
                owner.requests.append(request)
                messages = request.get("messages", [])
                tool_turns = [m for m in messages if m.get("role") == "tool"]
                if len(tool_turns) >= 2:
                    message = {"role": "assistant",
                               "content": "final after bounded MCP errors"}
                    finish = "stop"
                else:
                    message = {"role": "assistant", "tool_calls": [{
                        "id": "call_mcp", "type": "function", "function": {
                            "name": "fake:greet",
                            "arguments": json.dumps({"who": owner.argument}),
                        }
                    }]}
                    finish = "tool_calls"
                body = json.dumps({"choices": [{
                    "finish_reason": finish, "message": message
                }]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}/v1/chat/completions"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Harness:
    def __init__(self):
        self.temp = Path(tempfile.mkdtemp(prefix="motiris-mcp-"))
        self.server = self.temp / "server.py"
        self.server.write_text(SERVER_SOURCE)
        self.sequence = 0
        self.last_parent_fds = []
        self.last_parent_rss = []

    def close(self):
        shutil.rmtree(self.temp)

    def run(self, modes, provider=None, timeout_value="1"):
        self.sequence += 1
        self.last_parent_fds = []
        self.last_parent_rss = []
        home = self.temp / f"home-{self.sequence}"
        home.mkdir()
        pidfiles = [self.temp / f"pid-{self.sequence}-{i}" for i in range(len(modes))]
        servers = [{
            "name": "fake" if len(modes) == 1 else f"fake{i}",
            "cmd": sys.executable,
            "args": [str(self.server), mode, str(pidfiles[i])],
        } for i, mode in enumerate(modes)]
        config = {"transport": "echo", "mcp_servers": servers}
        if provider is not None:
            config.update(transport="libcurl", base_url=provider.url)
        (home / "config.json").write_text(json.dumps(config))

        env = {k: v for k, v in os.environ.items() if not k.startswith("MOTIRIS_")}
        env.update(MOTIRIS_HOME=str(home), MOTIRIS_MCP_TIMEOUT_S=timeout_value)
        command = [str(BIN), "-p", "hi", "--max-steps", "5", "-k", "x", "--no-plugin"]
        start = time.monotonic()
        proc = subprocess.Popen(command, cwd=ROOT, env=env, text=True,
                                stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            for pidfile in pidfiles:
                if pidfile.exists():
                    kill_from_pidfile(pidfile)
            os.killpg(proc.pid, signal.SIGKILL)
            try:
                stdout, stderr = proc.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                proc.stdout.close()
                proc.stderr.close()
                proc.wait(timeout=2)
                stdout, stderr = "", ""
            raise AssertionError(f"motiris did not finish: {modes}\n{stdout}\n{stderr}")
        elapsed = time.monotonic() - start
        for pidfile in pidfiles:
            if pidfile.exists():
                fd_count, rss_kib = assert_child_gone(pidfile)
                self.last_parent_fds.append(fd_count)
                self.last_parent_rss.append(rss_kib)
        assert proc.returncode == 0, (proc.returncode, stdout, stderr)
        assert "final after bounded MCP errors" in stdout or "echo round complete" in stdout, (
            stdout, stderr
        )
        return stdout, stderr, elapsed


def main():
    assert BIN.exists(), f"missing {BIN}"
    harness = Harness()
    fds_before = len(list(Path("/proc/self/fd").iterdir()))
    try:
        # Several failing servers in one process exercise repeated fd/child cleanup.
        _, err, elapsed = harness.run(["silent-init"] * 3)
        assert err.count("initialize failed: mcp request read timed out after 1 s") == 3, err
        assert 2.5 <= elapsed < 9, elapsed
        repeated_parent_fds = harness.last_parent_fds
        repeated_parent_rss = harness.last_parent_rss
        assert len(repeated_parent_fds) == 3, repeated_parent_fds
        assert len(set(repeated_parent_fds)) == 1, repeated_parent_fds
        # ASAN quarantine and libc allocation arenas warm up on the first
        # failure. Require steady-state stability across the next two starts.
        assert max(repeated_parent_rss[1:]) - min(repeated_parent_rss[1:]) <= 512, repeated_parent_rss

        _, err, _ = harness.run(["silent-list"])
        assert "tools/list failed: mcp request read timed out after 1 s" in err, err

        # Closed server stdin must produce a bounded error, never fatal SIGPIPE.
        _, err, _ = harness.run(["sigpipe-after-init"])
        assert "initialized notification failed: mcp server closed stdin" in err, err

        # The group is still killed when its direct leader exits before cleanup.
        _, err, _ = harness.run(["orphan-after-init"])
        assert "failed:" in err, err

        # Empty registration invokes close; SIGTERM ignore must escalate to SIGKILL.
        _, err, elapsed = harness.run(["empty-ignore-term"])
        assert "exposes no tools" in err, err
        assert 0.8 <= elapsed < 6, elapsed

        _, err, _ = harness.run(["oversize-init"])
        assert "initialize failed: mcp response exceeds 1048576 bytes" in err, err

        for mode, argument, expected in (
                ("silent-call", "motiris", "mcp request read timed out after 1 s"),
                ("stall-call-write", maximum_write_argument(),
                 "mcp request write timed out after 1 s")):
            provider = Provider(argument=argument)
            try:
                _, err, elapsed = harness.run([mode], provider=provider)
            finally:
                provider.close()
            assert "timed out" not in err  # tool errors are model-visible, not startup logs
            assert len(provider.requests) == 3, len(provider.requests)
            final_tools = [m for m in provider.requests[-1]["messages"]
                           if m.get("role") == "tool"]
            assert len(final_tools) == 2, final_tools
            for message in final_tools:
                content = json.loads(message["content"])
                assert content["error"] == expected, content
            assert elapsed < 7, elapsed  # the failed connection returns immediately on reuse

        start = time.monotonic()
        _, err, _ = harness.run(["silent-init"], timeout_value="0")
        assert time.monotonic() - start < 2
        assert "MOTIRIS_MCP_TIMEOUT_S must be 1..120" in err, err

        fds_after = len(list(Path("/proc/self/fd").iterdir()))
        assert fds_after == fds_before, (fds_before, fds_after)
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        print(f"mcp: test-driver max child RSS {rss} KiB; driver fd count "
              f"{fds_before} -> {fds_after}; motiris fds at repeated starts "
              f"{repeated_parent_fds}; motiris RSS {repeated_parent_rss} KiB")
        print("mcp: initialize/list/call read/write deadlines, response cap, "
              "SIGPIPE, descendant TERM/KILL cleanup, repeated failures and "
              "invalid config passed")
    finally:
        harness.close()


if __name__ == "__main__":
    main()
