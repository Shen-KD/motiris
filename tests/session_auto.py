#!/usr/bin/env python3
"""Automatic UUID session index/traces and safe REPL restore."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"


def invoke(env, text):
    proc = subprocess.run(
        [str(BIN), "-i", "--transport", "echo", "--no-plugin"],
        cwd=ROOT, env=env, input=text, text=True, capture_output=True, timeout=8)
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    return proc.stdout + proc.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-session-auto-") as tmp:
        root = Path(tmp)
        home, config, workspace = root / "home", root / "config", root / "workspace"
        home.mkdir(); config.mkdir(); workspace.mkdir()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MOTIRIS_")}
        env.update({"HOME": str(home), "MOTIRIS_HOME": str(config),
                    "MOTIRIS_WORKSPACE": str(workspace)})

        output = invoke(env, "first automatic turn\n/exit\n")
        index_path = config / "session/session.json"
        assert index_path.is_file(), output
        index = json.loads(index_path.read_text())
        assert index["version"] == 1 and len(index["sessions"]) == 1, index
        first = index["sessions"][0]
        assert first["kind"] == "repl" and first["id"] == index["active_session"]
        trace_path = config / "session" / first["file"]
        trace = json.loads(trace_path.read_text())
        assert trace["session_id"] == first["id"], trace
        assert trace["messages"][0]["content"] == "first automatic turn", trace
        assert all("reasoning" not in message and "reasoning_content" not in message
                   for message in trace["messages"]), trace

        output = invoke(env, "/load\n/exit\n")
        assert "loaded 4 messages" in output, output
        assert first["id"] in output, output

        output = invoke(env, "second session turn\n/new\n/load %s\n/exit\n" % first["id"])
        assert "new session" in output and "loaded 4 messages" in output, output
        index = json.loads(index_path.read_text())
        assert len(index["sessions"]) == 2, index
        assert index["active_session"] != first["id"], index
        ids = {entry["id"] for entry in index["sessions"]}
        assert first["id"] in ids and len(ids) == 2, ids
        for entry in index["sessions"]:
            assert (config / "session" / entry["file"]).is_file(), entry

        # A non-empty context cannot be replaced implicitly.
        output = invoke(env, "keep this context\n/load %s\n/exit\n" % first["id"])
        assert "requires an empty context" in output, output

        # A damaged index is not an empty index: fail without rewriting it or
        # silently treating older legacy logs as the authoritative session.
        index_path.write_text("{broken-index")
        output = invoke(env, "/load\n/exit\n")
        assert "session index unavailable or malformed" in output, output
        assert "loaded" not in output, output
        assert index_path.read_text() == "{broken-index"
    print("session-auto: atomic index, UUID traces, latest/selected restore and /new passed")


if __name__ == "__main__":
    main()
