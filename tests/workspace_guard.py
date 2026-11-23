#!/usr/bin/env python3
"""Per-Agent optional workspace boundary; unrestricted by default."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
next_call = {}


class Provider(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        messages = request["messages"]
        if messages[-1].get("role") == "tool":
            reply = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": messages[-1]["content"]}}]}
        else:
            reply = {"choices": [{"finish_reason": "tool_calls", "message": {
                "role": "assistant", "tool_calls": [{
                    "id": "workspace", "type": "function", "function": {
                        "name": next_call["name"],
                        "arguments": json.dumps(next_call["args"])}}]}}]}
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def run_tool(binary, home, workspace, port, name, args):
    next_call.clear()
    next_call.update(name=name, args=args)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update(MOTIRIS_HOME=str(home), MOTIRIS_WORKSPACE=str(workspace))
    proc = subprocess.run([str(binary), "-p", "file operation", "--no-plugin",
                           "--max-steps", "3", "-k", "x"],
                          env=env, text=True, capture_output=True, timeout=10)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    return json.loads(lines[-1])


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-workspace-guard-") as temp:
            root = Path(temp)
            workspace = root / "workspace"
            outside = root / "outside"
            home = root / "home"
            workspace.mkdir()
            outside.mkdir()
            home.mkdir()
            target = outside / "artifact.txt"
            target.write_text("OUTSIDE-ORIGINAL\n")
            config = {
                "transport": "libcurl",
                "base_url": "http://127.0.0.1:%d/v1/chat/completions" %
                            server.server_address[1],
                "workspace_guard": False,
                "approval": "off",
            }
            (home / "config.json").write_text(json.dumps(config))
            binary = ROOT / "motiris"
            port = server.server_address[1]

            result = run_tool(binary, home, workspace, port, "read_file",
                              {"path": str(target)})
            assert "OUTSIDE-ORIGINAL" in result["content"], result
            result = run_tool(binary, home, workspace, port, "write_file",
                              {"path": str(target), "content": "OUTSIDE-WRITE\n"})
            assert result["ok"] == "written" and target.read_text() == "OUTSIDE-WRITE\n", result
            result = run_tool(binary, home, workspace, port, "patch",
                              {"path": str(target), "old": "WRITE", "new": "PATCH"})
            assert result["replaced"] is True and target.read_text() == "OUTSIDE-PATCH\n", result
            result = run_tool(binary, home, workspace, port, "search_files",
                              {"path": str(outside), "pattern": "OUTSIDE-PATCH"})
            assert result["count"] == 1 and result["matches"][0]["file"] == str(target), result

            # Opting in restores the old boundary for every file operation.
            config["workspace_guard"] = True
            (home / "config.json").write_text(json.dumps(config))
            before = target.read_bytes()
            for name, args in [
                ("read_file", {"path": str(target)}),
                ("write_file", {"path": str(target), "content": "FORBIDDEN\n"}),
                ("patch", {"path": str(target), "old": "OUTSIDE", "new": "NO"}),
                ("search_files", {"path": str(outside), "pattern": "OUTSIDE"}),
            ]:
                result = run_tool(binary, home, workspace, port, name, args)
                assert "outside workspace" in result["error"], (name, result)
            assert target.read_bytes() == before
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    print("workspace-guard: unrestricted default and opt-in boundary passed")


if __name__ == "__main__":
    main()
