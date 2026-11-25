#!/usr/bin/env python3
"""Offline core/config contract tests for the interactive model card (#93)."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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

ROOT = Path(__file__).resolve().parents[1]

PROBE_SOURCE = r'''
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int motiris_register_core_tools_count(void) { return 0; }
const MotirisTool *motiris_core_tools(void) { return NULL; }
const char *motiris_tool_schema(const char *name) {
  (void)name;
  return NULL;
}
int motiris_register_mcp_server(MotirisAgent *a, const char *name,
                                const char *cmd, const char *const *args) {
  (void)a; (void)name; (void)cmd; (void)args;
  return 0;
}

static int setters(void) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  int ok = motiris_base_url(a) && *motiris_base_url(a) &&
           !strcmp(motiris_transport_name(a), "auto") &&
           !strcmp(motiris_think_mode(a), "provider") &&
           motiris_max_tokens(a) == 0 && !motiris_api_key_configured(a);
  if (motiris_set_think_mode(a, "on") || motiris_set_max_tokens(a, 123))
    ok = 0;
  if (motiris_set_think_mode(a, "invalid") != -1 ||
      strcmp(motiris_think_mode(a), "on") ||
      motiris_set_max_tokens(a, -1) != -1 || motiris_max_tokens(a) != 123)
    ok = 0;
  if (motiris_set_think_mode(a, NULL) ||
      strcmp(motiris_think_mode(a), "provider") ||
      motiris_set_think_mode(a, "") ||
      motiris_set_think_mode(a, "provider") ||
      motiris_set_max_tokens(a, 0) || motiris_max_tokens(a) != 0 ||
      motiris_set_max_tokens(a, 2147483647) ||
      motiris_max_tokens(a) != 2147483647)
    ok = 0;
  motiris_set_api_key(a, "agent-secret");
  if (!motiris_api_key_configured(a)) ok = 0;
  motiris_free(a);
  return ok ? 0 : 91;
}

static int cancel_now(void *ud) { (void)ud; return 1; }

static int fetch_with(const char *url, const char *agent_key,
                      const char *backend, int cancelled) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_set_base_url(a, url);
  motiris_set_transport(a, backend);
  if (cancelled) motiris_set_cancel_cb(a, cancel_now, NULL);
  if (agent_key) motiris_set_api_key(a, agent_key);
  cJSON *models = motiris_fetch_models(a);
  int rc = 0;
  if (!models) {
    fprintf(stderr, "%s\n", motiris_last_error(a));
    rc = 1;
  } else {
    char *printed = cJSON_PrintUnformatted(models);
    if (!printed) rc = 97;
    else { puts(printed); free(printed); }
    cJSON_Delete(models);
  }
  motiris_free(a);
  return rc;
}

static int env_set(const char *key, const char *value) {
  return motiris_set_env(key, value) ? 1 : 0;
}

static int idle(const char *url) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_set_base_url(a, url);
  motiris_set_transport(a, "libcurl");
  motiris_free(a);
  return 0;
}

static int config_roundtrip(void) {
  if (motiris_set_config("think_mode", "on") ||
      motiris_set_config("max_tokens", "4096")) return 94;
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_apply_config(a);
  int ok = !strcmp(motiris_think_mode(a), "on") &&
           motiris_max_tokens(a) == 4096;
  motiris_free(a);
  if (motiris_set_config("think_mode", "provider") ||
      motiris_set_config("max_tokens", "0")) return 95;
  a = motiris_new();
  if (!a) return 90;
  motiris_apply_config(a);
  ok = ok && !strcmp(motiris_think_mode(a), "provider") &&
       motiris_max_tokens(a) == 0;
  motiris_free(a);
  return ok ? 0 : 96;
}

static int request(const char *url, const char *mode, const char *tokens) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_set_base_url(a, url);
  motiris_set_transport(a, "libcurl");
  motiris_set_stream(a, 0);
  motiris_set_tools_enabled(a, 0);
  motiris_set_plugins_enabled(a, 0);
  if (mode && motiris_set_think_mode(a, mode)) {
    motiris_free(a);
    return 92;
  }
  if (tokens && motiris_set_max_tokens(a, atoi(tokens))) {
    motiris_free(a);
    return 93;
  }
  motiris_add_user(a, "model card request");
  int rc = motiris_run(a);
  if (rc) fprintf(stderr, "%s\n", motiris_last_error(a));
  motiris_free(a);
  return rc;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "setters")) return setters();
  if (argc == 2 && !strcmp(argv[1], "config-roundtrip"))
    return config_roundtrip();
  if (argc >= 3 && argc <= 4 && !strcmp(argv[1], "fetch"))
    return fetch_with(argv[2], argc > 3 ? argv[3] : NULL, "libcurl", 0);
  if (argc == 4 && !strcmp(argv[1], "fetch-backend"))
    return fetch_with(argv[2], NULL, argv[3], 0);
  if (argc == 3 && !strcmp(argv[1], "fetch-cancel"))
    return fetch_with(argv[2], NULL, "libcurl", 1);
  if (argc == 3 && !strcmp(argv[1], "idle")) return idle(argv[2]);
  if (argc == 4 && !strcmp(argv[1], "env-set"))
    return env_set(argv[2], argv[3]);
  if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "request"))
    return request(argv[2], argc > 3 ? argv[3] : NULL,
                   argc > 4 ? argv[4] : NULL);
  return 64;
}
'''


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.responses = {}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append({
            "method": "GET", "path": self.path, "body": raw,
            "accept": self.headers.get_all("Accept") or [],
            "authorization": self.headers.get_all("Authorization") or [],
        })
        body = json.dumps({"data": [{"id": "model one"},
                                    {"id": "org/exact:model"}]}).encode()
        status, body = self.server.responses.get(self.path, (200, body))
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append({"method": "POST", "path": self.path,
                                     "json": json.loads(raw)})
        body = json.dumps({"choices": [{"finish_reason": "stop", "message": {
            "role": "assistant", "content": "ok"}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def compile_probe(directory):
    source = directory / "model_card_probe.c"
    binary = directory / "model-card-probe"
    source.write_text(PROBE_SOURCE)
    subprocess.run([
        "gcc", "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-D_POSIX_C_SOURCE=200809L", "-D_GNU_SOURCE",
        "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/vendor"),
        "-I" + str(ROOT / "deps/libcurl/include"), str(source),
        str(ROOT / "src/core/agent.c"), str(ROOT / "src/core/transport.c"),
        str(ROOT / "src/core/config.c"), str(ROOT / "src/vendor/cJSON.c"),
        "-Wl,-l:libcurl.so.4", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    return binary


def environment(home):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({"HOME": str(home), "NO_PROXY": "127.0.0.1,localhost",
                "no_proxy": "127.0.0.1,localhost"})
    home.mkdir(parents=True, exist_ok=True)
    return env


def invoke(binary, env, *args):
    return subprocess.run([str(binary), *args], cwd=ROOT, env=env, text=True,
                          capture_output=True, timeout=10)


def test_request_contract(binary, env, provider):
    url = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port
    cases = [
        ((), None, None),
        (("provider", "0"), None, None),
        (("on", "4096"), True, 4096),
        (("off", "2147483647"), False, 2147483647),
    ]
    for args, think, tokens in cases:
        before = len(provider.requests)
        proc = invoke(binary, env, "request", url, *args)
        assert proc.returncode == 0, (args, proc.stdout, proc.stderr)
        assert len(provider.requests) == before + 1
        request = provider.requests[-1]["json"]
        if think is None:
            assert "think_mode" not in request, request
        else:
            assert request["think_mode"] is think, request
        if tokens is None:
            assert "max_tokens" not in request, request
        else:
            assert request["max_tokens"] == tokens, request
            assert type(request["max_tokens"]) is int, request


def test_discovery_success(binary, env, provider):
    root_url = "http://127.0.0.1:%d" % provider.server_port
    before = len(provider.requests)
    proc = invoke(binary, env, "idle", root_url + "/v1/chat/completions")
    assert proc.returncode == 0
    assert len(provider.requests) == before, "idle agent performed network I/O"

    cases = [
        ("/v1/chat/completions", "/v1/models"),
        ("/v1/chat/completions///?ignored=1#fragment", "/v1/models"),
        ("/v1", "/v1/models"),
        ("/v1/", "/v1/models"),
        ("/v1/models", "/v1/models"),
        ("/v1/models/?ignored=1", "/v1/models"),
        ("/chat/completions", "/models"),
    ]
    for endpoint, expected_path in cases:
        proc = invoke(binary, env, "fetch", root_url + endpoint)
        assert proc.returncode == 0, (endpoint, proc.stdout, proc.stderr)
        assert json.loads(proc.stdout) == ["model one", "org/exact:model"]
        request = provider.requests[-1]
        assert request["method"] == "GET" and request["path"] == expected_path
        assert request["body"] == b"" and request["accept"] == ["application/json"]
        assert request["authorization"] == [], request

    keyed = env.copy()
    keyed["MOTIRIS_API_KEY"] = "environment-secret"
    proc = invoke(binary, keyed, "fetch", root_url + "/v1")
    assert proc.returncode == 0, proc.stderr
    assert provider.requests[-1]["authorization"] == ["Bearer environment-secret"]
    proc = invoke(binary, keyed, "fetch", root_url + "/v1", "agent-secret")
    assert proc.returncode == 0, proc.stderr
    assert provider.requests[-1]["authorization"] == ["Bearer agent-secret"]


def test_discovery_failures(binary, env, provider):
    root_url = "http://127.0.0.1:%d" % provider.server_port
    path = "/v1/models"

    for status in (401, 404):
        secret = "response-secret-%d" % status
        provider.responses[path] = (status, json.dumps({
            "error": secret, "data": [{"id": "must-not-be-returned"}]
        }).encode())
        keyed = env.copy()
        keyed["MOTIRIS_API_KEY"] = "request-secret-%d" % status
        proc = invoke(binary, keyed, "fetch", root_url + "/v1")
        assert proc.returncode != 0, (status, proc.stdout)
        assert ("http status %d" % status) in proc.stderr, proc.stderr
        assert secret not in proc.stderr and keyed["MOTIRIS_API_KEY"] not in proc.stderr
        assert len(proc.stderr) < 512

    for body in (b"{", b"[]", b'{"data":{}}', b'{"data":[]}',
                 b'{"data":[null,{},{"id":7},{"id":""}]}'):
        provider.responses[path] = (200, body)
        proc = invoke(binary, env, "fetch", root_url + "/v1")
        assert proc.returncode != 0, (body, proc.stdout, proc.stderr)
        assert proc.stdout == "" and len(proc.stderr) < 512

    provider.responses[path] = (200, json.dumps({"data": [
        None, {}, {"id": 7}, {"id": ""}, {"id": "x" * 513},
        {"id": "y" * 512}, {"id": "valid"},
    ]}).encode())
    proc = invoke(binary, env, "fetch", root_url + "/v1")
    assert proc.returncode == 0, proc.stderr
    assert json.loads(proc.stdout) == ["y" * 512, "valid"]

    provider.responses[path] = (200, json.dumps({
        "data": [{"id": "model-%d" % i} for i in range(140)]
    }).encode())
    proc = invoke(binary, env, "fetch", root_url + "/v1")
    assert proc.returncode == 0, proc.stderr
    models = json.loads(proc.stdout)
    assert models == ["model-%d" % i for i in range(128)], len(models)

    provider.responses[path] = (200, b"S" * (1024 * 1024 + 1))
    proc = invoke(binary, env, "fetch", root_url + "/v1")
    assert proc.returncode != 0 and "response limit exceeded" in proc.stderr
    assert "S" * 100 not in proc.stderr and len(proc.stderr) < 512

    provider.responses[path] = (302, json.dumps({
        "data": [{"id": "redirect-must-not-succeed"}]
    }).encode())
    before = len(provider.requests)
    proc = invoke(binary, env, "fetch", root_url + "/v1")
    assert proc.returncode != 0 and "http status 302" in proc.stderr
    assert len(provider.requests) == before + 1

    provider.responses.pop(path, None)
    for backend in ("echo", "curl"):
        before = len(provider.requests)
        proc = invoke(binary, env, "fetch-backend", root_url + "/v1", backend)
        assert proc.returncode != 0 and "auto/libcurl" in proc.stderr
        assert len(provider.requests) == before

    before = len(provider.requests)
    proc = invoke(binary, env, "fetch-cancel", root_url + "/v1")
    assert proc.returncode != 0 and "cancelled" in proc.stderr
    assert len(provider.requests) == before

    secret = "L" * 490
    keyed = env.copy()
    keyed["MOTIRIS_API_KEY"] = secret
    before = len(provider.requests)
    proc = invoke(binary, keyed, "fetch", root_url + "/v1")
    assert proc.returncode != 0 and "authorization header too long" in proc.stderr
    assert len(provider.requests) == before and secret not in proc.stderr

    before = len(provider.requests)
    proc = invoke(binary, env, "fetch", "file:///tmp/not-http")
    assert proc.returncode != 0 and "HTTP(S)" in proc.stderr
    assert len(provider.requests) == before


def test_env_updates(binary, env, root):
    env = env.copy()
    config = root / "env-config"
    config.mkdir()
    env["MOTIRIS_HOME"] = str(config)
    path = config / "env"
    path.write_bytes(
        b"# retained comment\nMOTIRIS_MODEL=file-model\n"
        b"UNRELATED=keep\nMOTIRIS_API_KEY=old-secret\n"
        b"MOTIRIS_LITERAL=${KEEP_THIS}\nMOTIRIS_API_KEY=duplicate\n")
    path.chmod(0o640)
    proc = invoke(binary, env, "env-set", "MOTIRIS_API_KEY", "new-secret")
    assert proc.returncode == 0, (proc.stdout, proc.stderr)
    assert "new-secret" not in proc.stdout + proc.stderr
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert path.read_bytes() == (
        b"# retained comment\nMOTIRIS_MODEL=file-model\n"
        b"UNRELATED=keep\nMOTIRIS_API_KEY=new-secret\n"
        b"MOTIRIS_LITERAL=${KEEP_THIS}\n")
    assert not glob.glob(str(path) + ".tmp.*")

    proc = invoke(binary, env, "env-set", "MOTIRIS_OTHER", "abc$literal")
    assert proc.returncode == 0 and "abc$literal" not in proc.stdout + proc.stderr
    assert path.read_bytes().endswith(b"MOTIRIS_OTHER=abc$literal\n")
    before = path.read_bytes()
    invalid = [
        ("MOTIRIS_", "value"), ("API_KEY", "value"),
        ("MOTIRIS_API_KEY=EVIL", "value"), (" MOTIRIS_API_KEY", "value"),
        ("MOTIRIS_API_KEY ", "value"), ("MOTIRIS_API-KEY", "value"),
        ("MOTIRIS_API_KEY", ""), ("MOTIRIS_API_KEY", " leading"),
        ("MOTIRIS_API_KEY", "trailing "), ("MOTIRIS_API_KEY", "\tleading"),
        ("MOTIRIS_API_KEY", "trailing\t"), ("MOTIRIS_API_KEY", "line\nbreak"),
        ("MOTIRIS_API_KEY", "line\rbreak"), ("MOTIRIS_API_KEY", "${HOME}"),
        ("MOTIRIS_API_KEY", "prefix${UNFINISHED"),
        ("MOTIRIS_API_KEY", "L" * 490),
    ]
    for key, value in invalid:
        proc = invoke(binary, env, "env-set", key, value)
        assert proc.returncode != 0, (key, value)
        assert path.read_bytes() == before, (key, value)
        assert value not in proc.stdout + proc.stderr or not value
        assert not glob.glob(str(path) + ".tmp.*")

    secret = "K" * 489
    proc = invoke(binary, env, "env-set", "MOTIRIS_API_KEY", secret)
    assert proc.returncode == 0, proc.stderr
    assert secret not in proc.stdout + proc.stderr
    assert ("MOTIRIS_API_KEY=" + secret + "\n").encode() in path.read_bytes()

    def limit_file_size():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

    before = path.read_bytes()
    proc = subprocess.run([str(binary), "env-set", "MOTIRIS_API_KEY", "fault"],
                          cwd=ROOT, env=env, text=True, capture_output=True,
                          timeout=5, preexec_fn=limit_file_size)
    assert proc.returncode != 0
    assert path.read_bytes() == before and "fault" not in proc.stdout + proc.stderr
    assert not glob.glob(str(path) + ".tmp.*")

    fallback = env.copy()
    fallback.pop("MOTIRIS_HOME")
    fallback["HOME"] = str(root / "env-home")
    (root / "env-home").mkdir()
    proc = invoke(binary, fallback, "env-set", "MOTIRIS_API_KEY", "home-secret")
    assert proc.returncode == 0 and "home-secret" not in proc.stdout + proc.stderr
    fallback_path = root / "env-home/.motiris/env"
    assert fallback_path.read_bytes() == b"MOTIRIS_API_KEY=home-secret\n"
    assert stat.S_IMODE(fallback_path.stat().st_mode) == 0o600

    # Readers see complete old/new documents, never a partially rewritten line.
    errors = []
    stop = threading.Event()

    def read_repeatedly():
        while not stop.is_set():
            try:
                lines = fallback_path.read_text().splitlines()
                assert len(lines) == 1
                assert lines[0].startswith("MOTIRIS_API_KEY=home-")
                assert lines[0].endswith("-secret")
            except Exception as exc:
                errors.append(exc)
                return

    reader = threading.Thread(target=read_repeatedly)
    reader.start()
    try:
        for i in range(20):
            value = "home-%02d-secret" % i
            proc = invoke(binary, fallback, "env-set", "MOTIRIS_API_KEY", value)
            assert proc.returncode == 0
            assert value not in proc.stdout + proc.stderr
    finally:
        stop.set()
        reader.join(timeout=5)
    assert not reader.is_alive() and not errors, errors
    assert fallback_path.read_bytes() == b"MOTIRIS_API_KEY=home-19-secret\n"
    assert not glob.glob(str(fallback_path) + ".tmp.*")


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-model-card-core-") as tmp:
            root = Path(tmp)
            binary = compile_probe(root)
            env = environment(root / "home")
            env["MOTIRIS_HOME"] = str(root / "config")
            proc = invoke(binary, env, "setters")
            assert proc.returncode == 0, (proc.stdout, proc.stderr)
            test_request_contract(binary, env, provider)
            test_discovery_success(binary, env, provider)
            test_discovery_failures(binary, env, provider)
            proc = invoke(binary, env, "config-roundtrip")
            assert proc.returncode == 0, (proc.stdout, proc.stderr)
            test_env_updates(binary, env, root)
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("model-card-core: setters, config, discovery and atomic env updates passed")


if __name__ == "__main__":
    main()
