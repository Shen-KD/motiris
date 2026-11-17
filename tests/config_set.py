#!/usr/bin/env python3
"""Offline integration tests for `motiris set config KEY VALUE`."""

import glob
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "motiris"


def environment(home, config, use_config_home=True):
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("MOTIRIS_"):
            env.pop(key)
    env["HOME"] = str(home)
    if use_config_home:
        env["MOTIRIS_HOME"] = str(config)
    home.mkdir(parents=True, exist_ok=True)
    return env


def set_config(env, key, value):
    return subprocess.run([str(BIN), "set", "config", key, value], env=env,
                          text=True, capture_output=True, timeout=5)


def assert_unchanged(path, before, env, key, value):
    proc = set_config(env, key, value)
    assert proc.returncode != 0, (key, value, proc.stdout, proc.stderr)
    assert path.read_bytes() == before, (key, value)
    assert not glob.glob(str(path) + ".tmp.*"), (key, value)
    return proc


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-config-set-") as tmp:
        root = Path(tmp)
        home, config = root / "home", root / "config"
        path = config / "config.json"
        env = environment(home, config)

        proc = set_config(env, "model", "glm-5.3")
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        data = json.loads(path.read_text())
        assert data == {"model": "glm-5.3"}, data
        assert stat.S_IMODE(path.stat().st_mode) == 0o600
        assert "glm-5.3" not in proc.stdout + proc.stderr

        data.update({
            "providers": [{"name": "keep", "model": "p", "base_url": "local"}],
            "mcp_servers": [{"name": "keep", "cmd": "mock", "args": []}],
            "extension": {"preserve": [1, True, None]},
            "system": "old",
        })
        path.write_text(json.dumps(data))
        for key, value in [("stream", "false"), ("tools", "true"),
                           ("plugins", "false"), ("goal_check", "true")]:
            proc = set_config(env, key, value)
            assert proc.returncode == 0, (key, proc.stdout, proc.stderr)
        proc = set_config(env, "max_steps", "25")
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        data = json.loads(path.read_text())
        assert data["stream"] is False and data["tools"] is True
        assert data["plugins"] is False and data["goal_check"] is True
        assert data["max_steps"] == 25 and type(data["max_steps"]) is int
        assert data["providers"][0]["name"] == "keep"
        assert data["mcp_servers"][0]["cmd"] == "mock"
        assert data["extension"] == {"preserve": [1, True, None]}

        escaped = 'first line\n"quoted" \\ unicode: ☃'
        proc = set_config(env, "system", escaped)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        assert json.loads(path.read_text())["system"] == escaped
        assert escaped not in proc.stdout + proc.stderr
        proc = set_config(env, "transport", "libcurl")
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        assert json.loads(path.read_text())["transport"] == "libcurl"
        for key, value in [
            ("base_url", "https://example.test/v1/chat/completions?x=1&y=2"),
            ("api_key_env", "CUSTOM_MOTIRIS_KEY"),
            ("plugin_dir", "/tmp/motiris-plugins"),
            ("shell_allow", "git,ls"), ("shell_deny", "rm,sudo"),
        ]:
            proc = set_config(env, key, value)
            assert proc.returncode == 0, (key, proc.stdout, proc.stderr)
            assert json.loads(path.read_text())[key] == value

        for key, value in [("max_steps", "0"), ("max_steps", "-1"),
                           ("max_steps", "12x"), ("max_steps", "2147483648"),
                           ("stream", "yes"), ("transport", "bogus"),
                           ("model", ""), ("unknown", "value")]:
            assert_unchanged(path, path.read_bytes(), env, key, value)
        before = path.read_bytes()
        proc = assert_unchanged(path, before, env, "api_key", "dummy-secret")
        assert "dummy-secret" not in proc.stdout + proc.stderr

        # A real temporary-file write failure must preserve the original and
        # clean the partial temp file. Ignore SIGXFSZ so write returns EFBIG.
        def limit_file_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

        before = path.read_bytes()
        proc = subprocess.run([str(BIN), "set", "config", "model", "too-big"],
                              env=env, text=True, capture_output=True, timeout=5,
                              preexec_fn=limit_file_size)
        assert proc.returncode != 0, (proc.stdout, proc.stderr)
        assert path.read_bytes() == before
        assert not glob.glob(str(path) + ".tmp.*")

        for malformed in [b"", b'{"model":', b"[]", b"x" * (1024 * 1024 + 1)]:
            path.write_bytes(malformed)
            assert_unchanged(path, malformed, env, "model", "replacement")

        path.unlink()
        path.mkdir()
        proc = set_config(env, "model", "replacement")
        assert proc.returncode != 0
        assert path.is_dir()
        assert not glob.glob(str(path) + ".tmp.*")
        path.rmdir()
        os.mkfifo(path)
        proc = set_config(env, "model", "replacement")
        assert proc.returncode != 0
        assert stat.S_ISFIFO(path.stat().st_mode)
        assert not glob.glob(str(path) + ".tmp.*")
        path.unlink()

        # HOME fallback when MOTIRIS_HOME is absent.
        fallback_home = root / "fallback-home"
        fallback_env = environment(fallback_home, None, use_config_home=False)
        proc = set_config(fallback_env, "model", "home-model")
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        fallback_path = fallback_home / ".motiris" / "config.json"
        assert json.loads(fallback_path.read_text()) == {"model": "home-model"}

        # Readers never observe a partial JSON document across replacements.
        errors = []
        stop = threading.Event()

        def read_repeatedly():
            expected = {"home-model"} | {"model-%d" % i for i in range(30)}
            while not stop.is_set():
                try:
                    current = json.loads(fallback_path.read_text())
                    assert current["model"] in expected, current
                except Exception as exc:  # collected in the reader thread
                    errors.append(exc)
                    return

        reader = threading.Thread(target=read_repeatedly)
        reader.start()
        try:
            for i in range(30):
                proc = set_config(fallback_env, "model", "model-%d" % i)
                assert proc.returncode == 0, (i, proc.stdout, proc.stderr)
        finally:
            stop.set()
            reader.join(timeout=5)
        assert not reader.is_alive()
        assert not errors, errors
        assert json.loads(fallback_path.read_text())["model"] == "model-29"
        assert not glob.glob(str(fallback_path) + ".tmp.*")
        assert stat.S_IMODE(fallback_path.stat().st_mode) == 0o600

        bad = subprocess.run([str(BIN), "set", "config", "model"],
                             env=fallback_env, text=True, capture_output=True,
                             timeout=5)
        assert bad.returncode == 2
        assert json.loads(fallback_path.read_text())["model"] == "model-29"

    print("config-set: typed updates, preservation, atomic replacement and failures passed")


if __name__ == "__main__":
    main()
