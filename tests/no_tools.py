#!/usr/bin/env python3
"""Offline execution-policy regressions for CLI tools, skills, plugins and MCP."""

import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

ROOT = Path(os.environ.get("ROOT", Path(__file__).resolve().parent.parent))
BINARY = ROOT / "motiris"


def response(content=None, calls=None, role="assistant"):
    message = {"role": role}
    if content is not None:
        message["content"] = content
    if calls:
        message["tool_calls"] = [
            {"id": "call_%d" % i, "type": "function",
             "function": {"name": name, "arguments": json.dumps(arguments)}}
            for i, (name, arguments) in enumerate(calls)
        ]
    return {"choices": [{"finish_reason": "tool_calls" if calls else "stop",
                         "message": message}]}


class Provider:
    def __init__(self):
        self.requests = []
        self.responder = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        assert self.ready.wait(3), "provider did not start"
        self.url = "http://127.0.0.1:%d/v1/chat/completions" % self.port

    def run(self):
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                request = json.loads(self.rfile.read(
                    int(self.headers["Content-Length"])))
                outer.requests.append(request)
                body = json.dumps(outer.responder(request)).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.ready.set()
        self.server.serve_forever()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def environment(home, extra=None):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({"HOME": str(home), "MOTIRIS_HOME": str(home),
                "MOTIRIS_BIN": str(BINARY),
                "NO_PROXY": "127.0.0.1,localhost",
                "no_proxy": "127.0.0.1,localhost"})
    if extra:
        env.update(extra)
    return env


def write_mcp_marker_script(path, marker, append=False):
    path.write_text("""import json
from pathlib import Path
import sys
marker = Path(%r)
with marker.open(%r) as stream:
    stream.write('started\\n')
for line in sys.stdin:
    request = json.loads(line)
    method = request.get('method')
    if method == 'initialize':
        reply = {'jsonrpc': '2.0', 'id': request.get('id'),
                 'result': {'serverInfo': {'name': 'policy-probe'}}}
    elif method == 'tools/list':
        reply = {'jsonrpc': '2.0', 'id': request.get('id'),
                 'result': {'tools': []}}
    else:
        continue
    sys.stdout.write(json.dumps(reply) + '\\n')
    sys.stdout.flush()
    if method == 'tools/list':
        break
""" % (str(marker), "a" if append else "w"))
    path.chmod(0o755)


def run_disabled_policy(root, provider):
    home = root / "disabled-home"
    home.mkdir()
    plugin_dir = root / "plugins"
    plugin_dir.mkdir()
    shutil.copy(ROOT / "examples" / "hello_plugin.so", plugin_dir)
    mcp_marker = root / "mcp-disabled-started"
    mcp_script = root / "mcp_disabled.py"
    write_mcp_marker_script(mcp_script, mcp_marker)
    shell_marker = root / "shell-tool-executed"
    # The old unquoted popen command would evaluate this substitution in cwd.
    skill_dir = root / "skills$(touch${IFS}NO_TOOLS_SHELL_PWNED)"
    skill_dir.mkdir()
    (skill_dir / "notes.md").write_text(
        "---\nname: notes\ndescription: metadata-only\n---\nTEXT_ONLY_SKILL_SENTINEL\n")
    (home / "config.json").write_text(json.dumps({
        "transport": "libcurl", "base_url": provider.url,
        "tools": True, "plugins": True,
        "mcp_servers": [{"name": "forbidden", "cmd": "python3",
                         "args": [str(mcp_script)]}],
    }))

    def disabled_responder(request):
        if request["messages"][-1].get("role") == "tool":
            return response("DISABLED_POLICY_OK")
        return response(calls=[
            ("skill_list", {}),
            ("hello", {"name": "blocked"}),
            ("forbidden:remote", {}),
            ("shell", {"command": "touch %s" % shell_marker}),
        ])

    provider.responder = disabled_responder
    proc = subprocess.run(
        [str(BINARY), "-p", "disabled policy", "--no-tools",
         "--skill-dir", str(skill_dir), "--plugin-dir", str(plugin_dir),
         "--max-steps", "3", "-k", "x"],
        cwd=root, env=environment(home), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    assert proc.returncode == 0, proc.stdout
    assert "DISABLED_POLICY_OK" in proc.stdout, proc.stdout
    assert "hello plugin:" not in proc.stdout, proc.stdout
    assert not mcp_marker.exists(), "disabled MCP server was started"
    assert not shell_marker.exists(), "disabled shell tool was executed"
    assert not (root / "NO_TOOLS_SHELL_PWNED").exists(), \
        "text-only skill injection invoked a shell"
    assert len(provider.requests) == 2, provider.requests
    assert all("tools" not in request for request in provider.requests), \
        provider.requests
    system = [m.get("content", "") for m in provider.requests[0]["messages"]
              if m.get("role") == "system"]
    assert any("Available skills" in text and "metadata-only" in text
               for text in system), system
    assert all("TEXT_ONLY_SKILL_SENTINEL" not in text for text in system), system
    tool_results = [m for m in provider.requests[1]["messages"]
                    if m.get("role") == "tool"]
    assert len(tool_results) == 4, tool_results
    assert all("unknown tool" in m.get("content", "") for m in tool_results), \
        tool_results


def run_subagent_isolation(root, provider):
    home = root / "subagent-home"
    home.mkdir()
    plugin_dir = root / "subagent-plugins"
    plugin_dir.mkdir()
    shutil.copy(ROOT / "examples" / "hello_plugin.so", plugin_dir)
    mcp_starts = root / "mcp-subagent-starts"
    mcp_script = root / "mcp_subagent.py"
    write_mcp_marker_script(mcp_script, mcp_starts, append=True)
    (home / "config.json").write_text(json.dumps({
        "transport": "libcurl", "base_url": provider.url,
        "tools": True, "plugins": True,
        "mcp_servers": [{"name": "parent-only", "cmd": "python3",
                         "args": [str(mcp_script)]}],
    }))
    provider.requests.clear()

    def isolation_responder(request):
        messages = request["messages"]
        if any(m.get("role") == "user" and m.get("content") == "child isolation"
               for m in messages):
            return response("CHILD_ISOLATED_OK")
        if messages[-1].get("role") == "tool":
            return response("PARENT_ISOLATED_OK")
        return response(calls=[("subagent", {"task": "child isolation"})])

    provider.responder = isolation_responder
    proc = subprocess.run(
        [str(BINARY), "-p", "parent isolation", "--plugin-dir", str(plugin_dir),
         "--max-steps", "4", "-k", "x"],
        cwd=root, env=environment(home), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
    assert proc.returncode == 0, proc.stdout
    assert "PARENT_ISOLATED_OK" in proc.stdout, proc.stdout
    assert proc.stdout.count("hello plugin: tool 'hello' registered") == 1, \
        proc.stdout
    assert len(mcp_starts.read_text().splitlines()) == 1, \
        mcp_starts.read_text() if mcp_starts.exists() else "MCP did not start"
    child_requests = [request for request in provider.requests
                      if any(m.get("role") == "user" and
                             m.get("content") == "child isolation"
                             for m in request["messages"])]
    assert len(child_requests) == 1, provider.requests
    assert "tools" not in child_requests[0], child_requests[0]
    parent_requests = [request for request in provider.requests
                       if request not in child_requests]
    assert parent_requests and "tools" in parent_requests[0], parent_requests


def run_cron_disabled(root, provider):
    home = root / "cron-home"
    home.mkdir()
    mcp_marker = root / "mcp-cron-started"
    mcp_script = root / "mcp_cron.py"
    write_mcp_marker_script(mcp_script, mcp_marker)
    (home / "config.json").write_text(json.dumps({
        "transport": "libcurl", "base_url": provider.url,
        "tools": True,
        "mcp_servers": [{"name": "cron-forbidden", "cmd": "python3",
                         "args": [str(mcp_script)]}],
    }))
    jobs = root / "jobs.json"
    jobs.write_text(json.dumps([
        {"id": "disabled", "interval_s": 1, "prompt": "cron disabled"}
    ]))
    provider.requests.clear()
    provider.responder = lambda request: response("CRON_DISABLED_OK")
    proc = subprocess.run(
        [str(BINARY), "--cron", str(jobs), "--once", "--no-tools"],
        cwd=root, env=environment(home), text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    assert proc.returncode == 0, proc.stdout
    assert "CRON_DISABLED_OK" in proc.stdout, proc.stdout
    assert not mcp_marker.exists(), "cron --no-tools started configured MCP"
    assert len(provider.requests) == 1 and "tools" not in provider.requests[0], \
        provider.requests


def main():
    provider = Provider()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-no-tools-") as tmp:
            root = Path(tmp)
            run_disabled_policy(root, provider)
            run_subagent_isolation(root, provider)
            run_cron_disabled(root, provider)
    finally:
        provider.close()
    print("no-tools: CLI/cron, skill/plugin/MCP and subagent isolation passed")


if __name__ == "__main__":
    main()
