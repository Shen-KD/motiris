#!/usr/bin/env python3
"""Provider routing, keyless isolation and bounded-auth regressions."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"
DEAD_URL = "http://127.0.0.1:1/v1/chat/completions"


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.requests = []
        self.fail_models = set()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        request = json.loads(body)
        self.server.requests.append({
            "json": request,
            "authorization": self.headers.get_all("Authorization") or [],
        })
        if request.get("model") in self.server.fail_models:
            response = {"error": {"message": "planned provider failure"}}
        else:
            response = {"choices": [{"finish_reason": "stop", "message": {
                "role": "assistant", "content": "configured provider ok"}}]}
        encoded = json.dumps(response, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, *_args):
        pass


def run_agent(root, home, config, extra_env=None, verbose=False):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith((
               "MOTIRIS_", "PROVIDER_KEY_", "MISSING_PROVIDER_KEY_"))
           and key != "LONG_PROVIDER_KEY"}
    env.update({
        "HOME": str(home), "MOTIRIS_HOME": str(root),
        "MOTIRIS_API_KEY": "default-key",
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    })
    if extra_env:
        env.update(extra_env)
    (root / "config.json").write_text(json.dumps(config))
    command = [str(BINARY), "-p", "route", "--no-tools", "--no-plugin",
               "--max-steps", "1"]
    if verbose:
        command.append("-v")
    return subprocess.run(command, cwd=str(ROOT), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=10)


def assert_ok(proc, context):
    if proc.returncode or proc.stdout != "configured provider ok\n":
        raise AssertionError("%s failed: rc=%d\n%s\n%s" %
                             (context, proc.returncode, proc.stdout, proc.stderr))


def assert_request(request, model, authorization, context):
    if request["json"].get("model") != model:
        raise AssertionError("wrong model for %s: %r" %
                             (context, request["json"].get("model")))
    if request["authorization"] != authorization:
        raise AssertionError("wrong authorization for %s: %r" %
                             (context, request["authorization"]))


def provider_entry(url, model, name, key_name=None):
    entry = {"name": name, "model": model, "base_url": url}
    if key_name is not None:
        entry["api_key_env"] = key_name
    return entry


def main():
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="motiris-provider-") as tmp:
            root = Path(tmp)
            home = root / "home"
            home.mkdir()
            url = "http://127.0.0.1:%d/v1/chat/completions" % provider.server_port

            # Independently keyed entries retain their own credentials at
            # every live fallback hop, not just at the final successful entry.
            for count in (1, 2, 8):
                before = len(provider.requests)
                providers = []
                models = []
                extra_env = {}
                for index in range(count):
                    key_name = "PROVIDER_KEY_%d_%d" % (count, index)
                    model = "model-%d-%d" % (count, index)
                    models.append(model)
                    extra_env[key_name] = "key-%d-%d" % (count, index)
                    providers.append(provider_entry(
                        url, model, "p%d" % index, key_name))
                provider.fail_models = set(models[:-1])
                proc = run_agent(root, home, {
                    "transport": "libcurl", "model": "default-model",
                    "base_url": DEAD_URL, "providers": providers,
                }, extra_env)
                assert_ok(proc, "independently keyed count %d" % count)
                requests = provider.requests[before:]
                if len(requests) != count:
                    raise AssertionError("keyed count %d did not attempt every fallback" % count)
                for index, request in enumerate(requests):
                    assert_request(request, models[index],
                                   ["Bearer key-%d-%d" % (count, index)],
                                   "keyed fallback %d/%d" % (index, count))

            # NULL keys never inherit MOTIRIS_API_KEY. Exercise both an omitted
            # api_key_env and a named but missing variable, including real fallback.
            for mode in ("omitted", "missing-env"):
                for count in (1, 2, 8):
                    before = len(provider.requests)
                    providers = []
                    models = []
                    for index in range(count):
                        model = "%s-%d-%d" % (mode, count, index)
                        models.append(model)
                        key_name = ("MISSING_PROVIDER_KEY_%d_%d" % (count, index)
                                    if mode == "missing-env" else None)
                        providers.append(provider_entry(
                            url, model, "%s-p%d" % (mode, index), key_name))
                    provider.fail_models = set(models[:-1])
                    proc = run_agent(root, home, {
                        "transport": "libcurl", "model": "default-model",
                        "base_url": DEAD_URL, "providers": providers,
                    }, verbose=True)
                    assert_ok(proc, "%s keyless count %d" % (mode, count))
                    requests = provider.requests[before:]
                    if len(requests) != count:
                        raise AssertionError("%s count %d did not attempt every fallback" %
                                             (mode, count))
                    for index, request in enumerate(requests):
                        assert_request(request, models[index], [],
                                       "%s fallback %d/%d" % (mode, index, count))

            # A mixed chain must not forward one entry's key to another entry.
            before = len(provider.requests)
            models = ["mixed-keyless", "mixed-keyed", "mixed-missing"]
            provider.fail_models = set(models[:2])
            providers = [
                provider_entry(url, models[0], "keyless"),
                provider_entry(url, models[1], "keyed", "PROVIDER_KEY_MIXED"),
                provider_entry(url, models[2], "missing", "MISSING_PROVIDER_KEY_MIXED"),
            ]
            proc = run_agent(root, home, {
                "transport": "libcurl", "model": "default-model",
                "base_url": DEAD_URL, "providers": providers,
            }, {"PROVIDER_KEY_MIXED": "mixed-own-key"}, verbose=True)
            assert_ok(proc, "mixed keyless/keyed fallback")
            requests = provider.requests[before:]
            if len(requests) != 3:
                raise AssertionError("mixed fallback did not attempt every live entry")
            for index, authorization in enumerate(
                    ([], ["Bearer mixed-own-key"], [])):
                assert_request(requests[index], models[index], authorization,
                               "mixed fallback entry %d" % index)

            # The default provider still uses the default Agent/environment key.
            before = len(provider.requests)
            provider.fail_models = set()
            proc = run_agent(root, home, {
                "transport": "libcurl", "model": "default-model", "base_url": url,
            })
            assert_ok(proc, "default provider")
            requests = provider.requests[before:]
            if len(requests) != 1:
                raise AssertionError("default provider request count mismatch")
            assert_request(requests[0], "default-model", ["Bearer default-key"],
                           "default provider")

            # The fixed 512-byte header holds the 22-byte prefix plus at most
            # 489 key bytes. Longer keys fail before any network request.
            for key_length in (489, 490, 16384):
                before = len(provider.requests)
                secret = "s" * key_length
                proc = run_agent(root, home, {
                    "transport": "libcurl", "model": "default-model", "base_url": url,
                }, {"MOTIRIS_API_KEY": secret}, verbose=True)
                if key_length == 489:
                    assert_ok(proc, "maximum-length bearer key")
                    requests = provider.requests[before:]
                    if len(requests) != 1:
                        raise AssertionError("boundary bearer key request count mismatch")
                    assert_request(requests[0], "default-model", ["Bearer " + secret],
                                   "maximum-length bearer key")
                else:
                    if not proc.returncode:
                        raise AssertionError("overlong bearer key was silently accepted")
                    if provider.requests[before:]:
                        raise AssertionError("overlong bearer key reached the endpoint")
                    if "authorization header too long" not in proc.stderr:
                        raise AssertionError("missing bounded-auth diagnostic: %s" % proc.stderr)
                    if secret in proc.stdout or secret in proc.stderr:
                        raise AssertionError("overlong bearer key leaked to output")

            # Provider-list keys cover both sides of the boundary too. An
            # overlong later entry invalidates the chain before an earlier
            # healthy entry can perform network I/O.
            before = len(provider.requests)
            secret = "L" * 489
            proc = run_agent(root, home, {
                "transport": "libcurl", "model": "default-model", "base_url": DEAD_URL,
                "providers": [provider_entry(
                    url, "boundary-provider", "boundary", "LONG_PROVIDER_KEY")],
            }, {"LONG_PROVIDER_KEY": secret})
            assert_ok(proc, "provider-list maximum-length bearer key")
            requests = provider.requests[before:]
            if len(requests) != 1:
                raise AssertionError("provider-list boundary request count mismatch")
            assert_request(requests[0], "boundary-provider", ["Bearer " + secret],
                           "provider-list maximum-length bearer key")

            before = len(provider.requests)
            secret = "L" * 490
            proc = run_agent(root, home, {
                "transport": "libcurl", "model": "default-model", "base_url": DEAD_URL,
                "providers": [
                    provider_entry(url, "healthy-before-overlong", "healthy"),
                    provider_entry(url, "overlong-provider", "overlong",
                                   "LONG_PROVIDER_KEY"),
                ],
            }, {"LONG_PROVIDER_KEY": secret}, verbose=True)
            if not proc.returncode or provider.requests[before:]:
                raise AssertionError("overlong fallback chain was not rejected before I/O")
            if "authorization header too long" not in proc.stderr:
                raise AssertionError("provider-list bounded-auth diagnostic missing")
            if secret in proc.stdout or secret in proc.stderr:
                raise AssertionError("provider-list key leaked to output")
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=5)
    print("provider: routing, keyless isolation, fallback and bounded auth passed")


if __name__ == "__main__":
    main()
