#!/usr/bin/env python3
"""Default config/templates, auto-loaded skills and streamed reasoning."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"


def compact(value):
    return json.dumps(value, separators=(",", ":")).encode()


def sse(delta):
    return b"data: " + compact({"choices": [{"delta": delta}]}) + b"\n\n"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.skill_path = None


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(request)
        if self.path == "/read-skill":
            if request["messages"][-1].get("role") == "tool":
                body = compact({"choices": [{"finish_reason": "stop", "message": {
                    "role": "assistant", "content": "skill read ok"}}]})
            else:
                call = {"id": "read-skill", "type": "function", "function": {
                    "name": "read_file",
                    "arguments": json.dumps({"path": self.server.skill_path})}}
                body = compact({"choices": [{"finish_reason": "tool_calls",
                    "message": {"role": "assistant", "tool_calls": [call]}}]})
            content_type = "application/json"
        elif self.path == "/stream":
            body = (sse({"reasoning_content": "think-"}) +
                    sse({"reasoning": "deeper", "content": "streamed "}) +
                    sse({"content": "answer"}) + b"data: [DONE]\n\n")
            content_type = "text/event-stream"
        else:
            body = compact({"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": "buffered answer"}}]})
            content_type = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def environment(home, config, workspace):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({
        "HOME": str(home), "MOTIRIS_HOME": str(config),
        "MOTIRIS_WORKSPACE": str(workspace),
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    })
    home.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    return env


def run(env, args, timeout=6, stdin=None):
    return subprocess.run([str(BIN), *args], cwd=ROOT, env=env, input=stdin,
                          text=True, capture_output=True, timeout=timeout)


def skill(path, name, description, marker):
    path.mkdir(parents=True, exist_ok=True)
    (path / "SKILL.md").write_text(
        "---\nname: %s\ndescription: %s\n---\n%s\n" %
        (name, description, marker))


def test_init_and_persistence(env, config):
    proc = run(env, ["--init"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    expected = ["env", "config.json", "AGENTS.md", "SOUL.md",
                "skills/motiris-config/SKILL.md",
                "skills/motiris-session/SKILL.md",
                "skills/motiris-context-compaction/SKILL.md"]
    paths = [config / name for name in expected]
    for path in paths:
        assert path.is_file(), path
    data = json.loads((config / "config.json").read_text())
    assert data["max_steps"] == 0 and data["stream"] is True, data
    assert data["approval"] == "off" and data["workspace_guard"] is False, data
    assert data["reasoning_effort"] == "medium", data
    assert data["show_thinking"] is False, data
    assert "motiris set config" in (config / "skills/motiris-config/SKILL.md").read_text()
    assert "MOTIRIS_PLUGIN_DIR=%s/plugins" % config in (config / "env").read_text()

    before = {path: path.read_bytes() for path in paths}
    again = run(env, ["--init"])
    assert again.returncode != 0, (again.stdout, again.stderr)
    assert "kept" in again.stderr or "exists" in again.stderr, again.stderr
    for path, content in before.items():
        assert path.read_bytes() == content, path


def test_streaming_and_system_docs(env, config, provider):
    (config / "AGENTS.md").write_text("AGENT_SENTINEL instructions")
    (config / "SOUL.md").write_text("SOUL_SENTINEL persona")
    data = json.loads((config / "config.json").read_text())
    port = provider.server_address[1]
    data.update({
        "transport": "libcurl",
        "base_url": "http://127.0.0.1:%d/stream" % port,
        "reasoning_effort": "high",
        "show_thinking": True,
        "system": "BASE_SYSTEM",
    })
    (config / "config.json").write_text(json.dumps(data))
    before = len(provider.requests)
    proc = run(env, ["-p", "hi", "--no-tools", "--no-plugin", "-k", "x"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    assert proc.stdout == "streamed answer\n", (proc.stdout, proc.stderr)
    assert "think-deeper" in proc.stderr, proc.stderr
    request = provider.requests[before]
    assert request["stream"] is True and request["reasoning_effort"] == "high", request
    system = [m["content"] for m in request["messages"] if m["role"] == "system"]
    assert len(system) == 1, system
    for marker in ("BASE_SYSTEM", "AGENT_SENTINEL", "SOUL_SENTINEL"):
        assert marker in system[0], system

    before = len(provider.requests)
    proc = run(env, ["-p", "hi", "--no-tools", "--no-plugin", "-k", "x",
                     "--no-stream", "--reasoning-effort", "low",
                     "-b", "http://127.0.0.1:%d/buffered" % port])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    assert proc.stdout == "buffered answer\n", (proc.stdout, proc.stderr)
    request = provider.requests[before]
    assert "stream" not in request and request["reasoning_effort"] == "low", request


def test_skill_precedence(env, config, workspace, provider):
    skill(config / "skills/shared", "shared", "user-version", "USER")
    skill(config / "skills/user-only", "user-only", "user-only-desc", "USER")
    skill(workspace / ".motiris/skills/shared", "shared", "workspace-version",
          "WORKSPACE")
    proc = run(env, ["-i", "--transport", "echo", "--no-plugin"],
               stdin="/skills\n/exit\n")
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    assert "workspace-version" in proc.stdout, proc.stdout
    assert "user-version" not in proc.stdout, proc.stdout
    assert "user-only" in proc.stdout and "motiris-config" in proc.stdout, proc.stdout

    data = json.loads((config / "config.json").read_text())
    data["base_url"] = "http://127.0.0.1:%d/read-skill" % provider.server_address[1]
    data["workspace_guard"] = True  # indexed user Skills retain a narrow read exception
    (config / "config.json").write_text(json.dumps(data))
    provider.skill_path = str(config / "skills/user-only/SKILL.md")
    before = len(provider.requests)
    proc = run(env, ["-p", "load the relevant skill", "-k", "x", "--no-plugin"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    first, second = provider.requests[before:before + 2]
    system = [m["content"] for m in first["messages"] if m["role"] == "system"][0]
    assert "Available skills" in system and provider.skill_path in system, system
    assert "user-only-desc" in system and "USER\n" not in system, system
    assert second["messages"][-1]["role"] == "tool", second
    assert "USER" in second["messages"][-1]["content"], second


def test_config_command(env):
    proc = run(env, ["--config", "approval", "ask"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    proc = run(env, ["--config", "max_steps", "0"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    proc = run(env, ["--config", "reasoning_effort", "low"])
    assert proc.returncode == 0, (proc.stdout, proc.stderr)


def main():
    if not BIN.is_file():
        raise AssertionError("build motiris first")
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-defaults-") as tmp:
            root = Path(tmp)
            config, home, workspace = root / "config", root / "home", root / "workspace"
            env = environment(home, config, workspace)
            test_init_and_persistence(env, config)
            test_streaming_and_system_docs(env, config, provider)
            test_skill_precedence(env, config, workspace, provider)
            test_config_command(env)
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("agent-defaults: init, templates, skills, SSE, reasoning and --config passed")


if __name__ == "__main__":
    main()
