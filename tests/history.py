#!/usr/bin/env python3
"""Offline per-agent live-history retention and replay-budget tests."""
import http.server
import json
import os
import subprocess
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def history_bytes(messages):
    return sum(len(compact(message).encode("utf-8")) for message in messages)


def invoke(args, env, expected=0, input_text=None):
    result = subprocess.run([str(BINARY)] + args, env=env, text=True,
                            input=input_text, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if expected is not None and result.returncode != expected:
        raise AssertionError("rc=%d, expected=%d\nstdout=%s\nstderr=%s" %
                             (result.returncode, expected,
                              result.stdout, result.stderr))
    return result


def model_turn(snapshot, prompt, env, resume=False, expected=0):
    args = []
    if resume:
        args += ["-r", str(snapshot)]
    args += ["-p", prompt, "--save", str(snapshot), "--transport", "libcurl",
             "--no-tools", "--no-plugin", "--max-steps", "4", "-v"]
    return invoke(args, env, expected=expected)


def users(snapshot):
    return [m["content"] for m in snapshot["messages"] if m["role"] == "user"]


class Provider(http.server.BaseHTTPRequestHandler):
    captures = []
    replay_only = False

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.captures.append(request)
        messages = request["messages"]
        if self.replay_only or messages[-1].get("role") == "tool":
            message = {"role": "assistant", "content": "round complete"}
            finish = "stop"
        else:
            message = {"role": "assistant", "tool_calls": [{
                "id": "call_history", "type": "function",
                "function": {"name": "shell", "arguments": "{}"}}]}
            finish = "tool_calls"
        body = compact({"choices": [{"finish_reason": finish,
                                     "message": message}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        home = root / "home"
        config = root / "config"
        home.mkdir()
        config.mkdir()
        base = os.environ.copy()
        base["HOME"] = str(home)
        base["MOTIRIS_HOME"] = str(config)
        for key in list(base):
            if key.startswith("MOTIRIS_") and key != "MOTIRIS_HOME":
                base.pop(key)

        provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        base["MOTIRIS_BASE_URL"] = (
            "http://127.0.0.1:%d/v1/chat/completions" % provider.server_address[1])
        try:
            # Item retention preserves complete four-message model/tool turns.
            item_env = base.copy()
            item_env["MOTIRIS_HISTORY_MAX_MESSAGES"] = "8"
            item_snapshot = root / "items.log"
            for turn in range(1, 4):
                result = model_turn(item_snapshot, "turn-%d" % turn, item_env,
                                    resume=turn > 1)
            saved = json.loads(item_snapshot.read_text())
            item_messages = saved["messages"]
            if len(item_messages) != 8 or users(saved) != ["turn-2", "turn-3"]:
                raise AssertionError("item retention split/lost turns: %r" % item_messages)
            if "history retention: evicted 4 messages" not in result.stderr:
                raise AssertionError("missing retention diagnostic: %s" % result.stderr)

            # The byte metric is exactly the sum of compact serialized records.
            probe = root / "probe.log"
            model_turn(probe, "byte-1", base)
            turn_bytes = history_bytes(json.loads(probe.read_text())["messages"])
            byte_env = base.copy()
            byte_env["MOTIRIS_HISTORY_MAX_BYTES"] = str(turn_bytes * 2)
            byte_snapshot = root / "bytes.log"
            for turn in range(1, 4):
                model_turn(byte_snapshot, "byte-%d" % turn, byte_env, resume=turn > 1)
            byte_messages = json.loads(byte_snapshot.read_text())["messages"]
            if (history_bytes(byte_messages) > turn_bytes * 2 or
                    users({"messages": byte_messages}) != ["byte-2", "byte-3"]):
                raise AssertionError("byte retention exceeded/split history: %r" %
                                     byte_messages)

            # An over-budget active turn fails closed rather than splitting its
            # assistant/tool sequence or silently truncating model content.
            small = base.copy()
            small["MOTIRIS_HISTORY_MAX_MESSAGES"] = "3"
            rejected = model_turn(root / "small.log", "too large", small, expected=1)
            if "history message budget exceeded for active turn" not in rejected.stderr:
                raise AssertionError("active-turn item diagnostic: %s" % rejected.stderr)
            tiny = base.copy()
            tiny["MOTIRIS_HISTORY_MAX_BYTES"] = "64"
            rejected = model_turn(root / "tiny.log", "too large", tiny, expected=1)
            if "history byte budget exceeded for active turn" not in rejected.stderr:
                raise AssertionError("active-turn byte diagnostic: %s" % rejected.stderr)

            # A smaller live budget is already enforced by snapshot restore.
            restore_env = base.copy()
            restore_env["MOTIRIS_HISTORY_MAX_MESSAGES"] = "4"
            restored = invoke(["-i", "--transport", "echo", "--no-tools", "--no-plugin"],
                              restore_env,
                              input_text="/resume %s\n/exit\n" % item_snapshot)
            if "loaded 4 messages" not in restored.stdout:
                raise AssertionError("restore did not retain expected messages: %s%s" %
                                     (restored.stdout, restored.stderr))

            # Replay is independent from live storage: retain all eight records
            # but send only the latest complete turn to a resumed provider.
            Provider.replay_only = True
            replay = base.copy()
            replay["MOTIRIS_REPLAY_MAX_MESSAGES"] = "4"
            before = len(Provider.captures)
            invoke(["-r", str(item_snapshot), "--transport", "libcurl",
                    "--no-tools", "--no-plugin"], replay)
            if Provider.captures[before]["messages"] != item_messages[-4:]:
                raise AssertionError("replay did not select one safe latest turn")
            if len(json.loads(item_snapshot.read_text())["messages"]) != 8:
                raise AssertionError("replay budget mutated the persisted live history")

            replay["MOTIRIS_REPLAY_MAX_MESSAGES"] = "3"
            before = len(Provider.captures)
            rejected = invoke(["-r", str(item_snapshot), "--transport", "libcurl",
                               "--no-tools", "--no-plugin"], replay, expected=1)
            if "replay message budget exceeded for active turn" not in rejected.stderr:
                raise AssertionError("replay item diagnostic: %s" % rejected.stderr)
            if len(Provider.captures) != before:
                raise AssertionError("over-budget replay reached the provider")

            replay_bytes = base.copy()
            replay_bytes["MOTIRIS_REPLAY_MAX_BYTES"] = str(
                history_bytes(item_messages[-4:]))
            before = len(Provider.captures)
            invoke(["-r", str(item_snapshot), "--transport", "libcurl",
                    "--no-tools", "--no-plugin"], replay_bytes)
            if Provider.captures[before]["messages"] != item_messages[-4:]:
                raise AssertionError("replay byte budget selected an unsafe prefix")
        finally:
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=5)
    print("history: live item/byte retention, safe turns and independent replay passed")


if __name__ == "__main__":
    main()
