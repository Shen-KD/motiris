#!/usr/bin/env python3
"""Provider retry, backoff, raw-error and cancellation regressions (#97)."""

from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "motiris"
DEAD_URL = "http://127.0.0.1:1/v1/chat/completions"


def compact(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def chat(text):
    return compact({"choices": [{"finish_reason": "stop", "message": {
        "role": "assistant", "content": text}}]})


def sse_message(text):
    return (b"data: " + compact({"choices": [{"delta": {
        "content": text}}]}) + b"\n\n" +
        b"data: " + compact({"choices": [{"delta": {},
        "finish_reason": "stop"}]}) + b"\n\ndata: [DONE]\n\n")


class Provider(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.routes = {}
        self.hits = Counter()
        self.requests = {}

    def add(self, path, responses):
        self.routes[path] = list(responses)

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.server_port, path)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        index = self.server.hits[self.path]
        self.server.hits[self.path] += 1
        request = json.loads(raw)
        self.server.requests.setdefault(self.path, []).append({
            "json": request,
            "authorization": self.headers.get_all("Authorization") or [],
        })
        responses = self.server.routes[self.path]
        response = responses[min(index, len(responses) - 1)]
        status, body = response[:2]
        content_type = response[2] if len(response) > 2 else "application/json"
        truncated = len(response) > 3 and response[3]
        if status == 0:
            self.close_connection = True
            self.connection.close()
            return
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body) + (64 if truncated else 0)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def log_message(self, *_args):
        pass


def agent_environment(config_home, config):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    home = config_home / "home"
    home.mkdir(exist_ok=True)
    env.update({
        "HOME": str(home), "MOTIRIS_HOME": str(config_home),
        "MOTIRIS_API_KEY": "retry-test-key",
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    })
    config_home.mkdir(exist_ok=True)
    (config_home / "config.json").write_text(json.dumps(config))
    return env


def run_agent(config_home, config, timeout=10, extra_args=(), extra_env=None):
    env = agent_environment(config_home, config)
    if extra_env:
        env.update(extra_env)
    return subprocess.run([
        str(BINARY), "-p", "retry route", "--no-tools", "--no-plugin",
        "--max-steps", "1", *extra_args,
    ], cwd=ROOT, env=env, text=True, capture_output=True, timeout=timeout)


def run_repl(config_home, config):
    env = agent_environment(config_home, config)
    return subprocess.run([
        str(BINARY), "-i", "--no-tools", "--no-plugin", "--max-steps", "1",
    ], cwd=ROOT, env=env, input="retry route\n/exit\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)


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

static unsigned int observed_delays[16];
static int observed_delay_count;
static int cancel_after_delay;
static int cancelled;

int motiris_test_retry_wait(MotirisAgent *a, unsigned int seconds) {
  (void)a;
  if (observed_delay_count < 16)
    observed_delays[observed_delay_count] = seconds;
  observed_delay_count++;
  if (cancel_after_delay) {
    cancelled = 1;
    return 1;
  }
  return 0;
}

static int cancel_requested(void *ud) {
  (void)ud;
  return cancelled;
}

static int setters(void) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  int ok = motiris_max_retries(a) == 0 &&
           motiris_retry_base_seconds(a) == 1;
  if (motiris_set_max_retries(a, 2) ||
      motiris_set_retry_base_seconds(a, 0))
    ok = 0;
  if (motiris_set_max_retries(a, -1) != -1 ||
      motiris_set_max_retries(a, 9) != -1 ||
      motiris_max_retries(a) != 2 ||
      motiris_set_retry_base_seconds(a, -1) != -1 ||
      motiris_set_retry_base_seconds(a, 61) != -1 ||
      motiris_retry_base_seconds(a) != 0)
    ok = 0;
  if (motiris_set_max_retries(a, 8) ||
      motiris_set_retry_base_seconds(a, 60) ||
      motiris_max_retries(a) != 8 ||
      motiris_retry_base_seconds(a) != 60)
    ok = 0;
  motiris_free(a);
  return ok ? 0 : 91;
}

static int show_config(int preserve) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  if (preserve && (motiris_set_max_retries(a, 6) ||
                   motiris_set_retry_base_seconds(a, 9))) {
    motiris_free(a);
    return 91;
  }
  motiris_apply_config(a);
  printf("%d %d\n", motiris_max_retries(a), motiris_retry_base_seconds(a));
  motiris_free(a);
  return 0;
}

static int config_reload(void) {
  if (motiris_set_config("max_retries", "5") ||
      motiris_set_config("retry_base_seconds", "7")) return 92;
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_apply_config(a);
  int ok = motiris_max_retries(a) == 5 &&
           motiris_retry_base_seconds(a) == 7;
  if (motiris_set_config("max_retries", "2") ||
      motiris_set_config("retry_base_seconds", "0")) ok = 0;
  motiris_apply_config(a);
  ok = ok && motiris_max_retries(a) == 2 &&
       motiris_retry_base_seconds(a) == 0;
  motiris_free(a);
  return ok ? 0 : 93;
}

static int run_request(const char *url, int retries, int base,
                       int cancel_at_delay) {
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_set_base_url(a, url);
  motiris_set_transport(a, "libcurl");
  motiris_set_stream(a, 0);
  motiris_set_tools_enabled(a, 0);
  motiris_set_plugins_enabled(a, 0);
  if (motiris_set_max_retries(a, retries) ||
      motiris_set_retry_base_seconds(a, base)) {
    motiris_free(a);
    return 91;
  }
  cancel_after_delay = cancel_at_delay;
  motiris_set_cancel_cb(a, cancel_requested, NULL);
  motiris_add_user(a, "backoff probe");
  int rc = motiris_run(a);
  printf("RESULT %d %d ", rc, cJSON_GetArraySize(motiris_messages(a)));
  for (int i = 0; i < observed_delay_count; i++)
    printf("%s%u", i ? "," : "", observed_delays[i]);
  printf("\nERROR %s\n", motiris_last_error(a));
  motiris_free(a);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "setters")) return setters();
  if (argc == 2 && !strcmp(argv[1], "config")) return show_config(0);
  if (argc == 2 && !strcmp(argv[1], "config-preserve")) return show_config(1);
  if (argc == 2 && !strcmp(argv[1], "config-reload")) return config_reload();
  if (argc == 6 && !strcmp(argv[1], "run"))
    return run_request(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
  return 64;
}
'''

NOFS_PROBE_SOURCE = r'''
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fake_hits;
static unsigned int host_delays[8];
static int host_delay_count;

static int host_wait(unsigned int seconds, void *ud) {
  (void)ud;
  if (host_delay_count < 8) host_delays[host_delay_count] = seconds;
  host_delay_count++;
  return 0;
}

static char *copy_text(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = malloc(size);
  if (copy) memcpy(copy, text, size);
  return copy;
}

int motiris_register_core_tools_count(void) { return 0; }
const MotirisTool *motiris_core_tools(void) { return NULL; }
const char *motiris_tool_schema(const char *name) {
  (void)name;
  return NULL;
}

char *motiris_transport_send(const char *backend, const char *url,
                             const char *auth, const char *body,
                             MotirisCancelCallback *cancel, void *cancel_ud,
                             char **err, char **error_body) {
  (void)backend; (void)url; (void)auth; (void)body;
  (void)cancel; (void)cancel_ud; (void)error_body;
  fake_hits++;
  if (fake_hits <= 2) {
    if (err) *err = copy_text("fake transport failure");
    return NULL;
  }
  return copy_text("{\"choices\":[{\"finish_reason\":\"stop\","
                   "\"message\":{\"role\":\"assistant\","
                   "\"content\":\"nofs ok\"}}]}");
}

char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud,
                                    void (*on_reasoning)(const char *, void *),
                                    void *reasoning_ud,
                                    MotirisCancelCallback *cancel,
                                    void *cancel_ud, char **err,
                                    char **error_body) {
  (void)on_delta; (void)ud; (void)on_reasoning; (void)reasoning_ud;
  return motiris_transport_send(backend, url, auth, body, cancel, cancel_ud,
                                err, error_body);
}

char *motiris_transport_get(const char *backend, const char *url,
                            const char *auth, MotirisCancelCallback *cancel,
                            void *cancel_ud, char **err) {
  (void)backend; (void)url; (void)auth; (void)cancel; (void)cancel_ud;
  if (err) *err = copy_text("unused");
  return NULL;
}

static int run_one(int base, int use_host_wait) {
  fake_hits = 0;
  host_delay_count = 0;
  MotirisAgent *a = motiris_new();
  if (!a) return 90;
  motiris_set_transport(a, "fake");
  motiris_set_stream(a, 0);
  motiris_set_tools_enabled(a, 0);
  if (motiris_set_max_retries(a, 2) ||
      motiris_set_retry_base_seconds(a, base)) return 91;
  if (use_host_wait) motiris_set_retry_wait_cb(a, host_wait, NULL);
  motiris_add_user(a, "strict nofs");
  int rc = motiris_run(a);
  printf("NOFS base=%d host=%d rc=%d hits=%d messages=%d delays=", base,
         use_host_wait, rc, fake_hits, cJSON_GetArraySize(motiris_messages(a)));
  for (int i = 0; i < host_delay_count; i++)
    printf("%s%u", i ? "," : "", host_delays[i]);
  printf(" error=%s\n", motiris_last_error(a));
  motiris_free(a);
  return 0;
}

int main(void) {
  if (run_one(0, 0) || run_one(1, 1) || run_one(1, 0)) return 1;
  return 0;
}
'''


def compile_probe(directory):
    source = directory / "provider_retry_probe.c"
    binary = directory / "provider-retry-probe"
    source.write_text(PROBE_SOURCE)
    subprocess.run([
        "gcc", "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-D_POSIX_C_SOURCE=200809L", "-D_GNU_SOURCE",
        "-DMOTIRIS_RETRY_WAIT_HOOK=motiris_test_retry_wait",
        "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/vendor"),
        "-I" + str(ROOT / "deps/libcurl/include"), str(source),
        str(ROOT / "src/core/agent.c"), str(ROOT / "src/core/transport.c"),
        str(ROOT / "src/core/config.c"), str(ROOT / "src/vendor/cJSON.c"),
        "-Wl,-l:libcurl.so.4", "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    return binary


class ProviderRetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="motiris-provider-retry-")
        cls.root = Path(cls.temp.name)
        cls.probe = compile_probe(cls.root)
        cls.provider = Provider()
        cls.provider_thread = threading.Thread(
            target=cls.provider.serve_forever, daemon=True)
        cls.provider_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.provider.shutdown()
        cls.provider.server_close()
        cls.provider_thread.join(timeout=5)
        cls.temp.cleanup()

    def invoke_probe(self, *args, env=None):
        return subprocess.run([str(self.probe), *args], cwd=ROOT, env=env,
                              text=True, capture_output=True, timeout=10)

    def config_env(self, directory):
        home = directory / "home"
        config = directory / "config"
        home.mkdir(exist_ok=True)
        config.mkdir(exist_ok=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MOTIRIS_")}
        env.update({"HOME": str(home), "MOTIRIS_HOME": str(config)})
        return env, config / "config.json"

    def test_public_retry_setters_and_defaults(self):
        proc = self.invoke_probe("setters")
        self.assertEqual(proc.returncode, 0, (proc.stdout, proc.stderr))

    def test_attempt_counts_success_exhaustion_and_fallback(self):
        case = self.root / "attempts"
        case.mkdir()
        failure = compact({"error": {"message": "planned retry"}})

        self.provider.add("/default", [(200, chat("default ok"))])
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False,
            "base_url": self.provider.url("/default"),
        })
        self.assertEqual((proc.returncode, proc.stdout), (0, "default ok\n"),
                         proc.stderr)
        self.assertEqual(self.provider.hits["/default"], 1)

        self.provider.add("/third", [
            (500, failure), (500, failure), (200, chat("third ok"))])
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False,
            "base_url": self.provider.url("/third"),
            "max_retries": 2, "retry_base_seconds": 0,
        })
        self.assertEqual((proc.returncode, proc.stdout), (0, "third ok\n"),
                         proc.stderr)
        self.assertEqual(self.provider.hits["/third"], 3)

        self.provider.add("/exhausted", [(500, failure)])
        self.provider.add("/fallback", [(200, chat("fallback ok"))])
        providers = [
            {"name": "exhausted", "model": "first",
             "base_url": self.provider.url("/exhausted")},
            {"name": "fallback", "model": "second",
             "base_url": self.provider.url("/fallback")},
        ]
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False, "providers": providers,
            "max_retries": 2, "retry_base_seconds": 0,
        })
        self.assertEqual((proc.returncode, proc.stdout), (0, "fallback ok\n"),
                         proc.stderr)
        self.assertEqual(self.provider.hits["/exhausted"], 3)
        self.assertEqual(self.provider.hits["/fallback"], 1)
        self.assertEqual([r["json"]["model"] for r in
                          self.provider.requests["/fallback"]], ["second"])
        self.assertEqual([r["json"]["model"] for r in
                          self.provider.requests["/exhausted"]],
                         ["first", "first", "first"])
        for path in ("/exhausted", "/fallback"):
            self.assertTrue(all(r["authorization"] == []
                                for r in self.provider.requests[path]))

        self.provider.add("/fail-one", [(500, failure)])
        self.provider.add("/fail-two", [(500, failure)])
        providers[0]["base_url"] = self.provider.url("/fail-one")
        providers[1]["base_url"] = self.provider.url("/fail-two")
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False, "providers": providers,
            "max_retries": 2, "retry_base_seconds": 0,
        })
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertEqual(self.provider.hits["/fail-one"], 3)
        self.assertEqual(self.provider.hits["/fail-two"], 3)

    def test_all_provider_failure_classes_retry_then_succeed(self):
        case = self.root / "failure-classes"
        case.mkdir()
        api_error = compact({"error": {"message": "retry api error"}})
        scenarios = {
            "transport": [(0, b""), (0, b""), (200, chat("transport ok"))],
            "http": [(503, api_error), (503, api_error),
                     (200, chat("http ok"))],
            "malformed": [(200, b"not-json\nsecond line"),
                          (200, b"not-json\nsecond line"),
                          (200, chat("malformed ok"))],
            "api": [(200, api_error), (200, api_error),
                    (200, chat("api ok"))],
            "choices": [(200, b'{"id":"missing-choices"}'),
                        (200, b'{"id":"missing-choices"}'),
                        (200, chat("choices ok"))],
            "message": [(200, b'{"choices":[{"finish_reason":"stop"}]}'),
                        (200, b'{"choices":[{"finish_reason":"stop"}]}'),
                        (200, chat("message ok"))],
            "null-message": [
                (200, b'{"choices":[{"message":null}]}'),
                (200, b'{"choices":[{"message":null}]}'),
                (200, chat("null-message ok"))],
        }
        for name, responses in scenarios.items():
            with self.subTest(name=name):
                path = "/class-" + name
                self.provider.add(path, responses)
                proc = run_agent(case, {
                    "transport": "libcurl", "stream": False,
                    "base_url": self.provider.url(path),
                    "max_retries": 2, "retry_base_seconds": 0,
                })
                self.assertEqual((proc.returncode, proc.stdout),
                                 (0, name + " ok\n"), proc.stderr)
                self.assertEqual(self.provider.hits[path], 3, proc.stderr)

    def test_strict_json_and_stream_protocol_failures_retry(self):
        case = self.root / "strict-stream"
        case.mkdir()
        trailing = chat("must not accept prefix") + b" trailing-garbage"
        self.provider.add("/trailing", [(200, trailing),
                                        (200, chat("strict json ok"))])
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False,
            "base_url": self.provider.url("/trailing"),
            "max_retries": 1, "retry_base_seconds": 0,
        })
        self.assertEqual((proc.returncode, proc.stdout),
                         (0, "strict json ok\n"), proc.stderr)
        self.assertEqual(self.provider.hits["/trailing"], 2)
        self.assertIn(trailing.decode(), proc.stderr)

        malformed = b'data: {"choices":[{"delta":{}}]} trailing\n\n'
        api_error = b'data: {"error":{"message":"sse failure"}}\n\n'
        incomplete = b'data: {"choices":[{"delta":{"role":"assistant"}}]}\n\n'
        for name, failure in (("malformed", malformed), ("api-error", api_error),
                              ("incomplete", incomplete)):
            with self.subTest(name=name):
                path = "/stream-" + name
                self.provider.add(path, [
                    (200, failure, "text/event-stream"),
                    (200, sse_message(name + " recovered"), "text/event-stream"),
                ])
                proc = run_agent(case, {
                    "transport": "libcurl", "stream": True,
                    "base_url": self.provider.url(path),
                    "max_retries": 1, "retry_base_seconds": 0,
                })
                self.assertEqual((proc.returncode, proc.stdout),
                                 (0, name + " recovered\n"), proc.stderr)
                self.assertEqual(self.provider.hits[path], 2, proc.stderr)
                self.assertIn(failure.decode(), proc.stderr)

    def test_backoff_sequence_full_shift_and_cancellation(self):
        case = self.root / "backoff"
        case.mkdir()
        env, _path = self.config_env(case)
        env.update({"NO_PROXY": "127.0.0.1,localhost",
                    "no_proxy": "127.0.0.1,localhost"})
        failure = compact({"error": {"message": "backoff failure"}})
        self.provider.add("/backoff-full", [(500, failure)])
        proc = self.invoke_probe(
            "run", self.provider.url("/backoff-full"), "8", "60", "0", env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.splitlines()[0],
                         "RESULT 1 1 60,120,240,480,960,1920,3840,7680",
                         proc.stdout + proc.stderr)
        self.assertEqual(self.provider.hits["/backoff-full"], 9)

        self.provider.add("/backoff-short", [(500, failure)])
        proc = self.invoke_probe(
            "run", self.provider.url("/backoff-short"), "2", "1", "0", env=env)
        self.assertEqual(proc.stdout.splitlines()[0], "RESULT 1 1 1,2",
                         proc.stdout + proc.stderr)
        self.assertEqual(self.provider.hits["/backoff-short"], 3)

        self.provider.add("/backoff-cancel", [(500, failure)])
        proc = self.invoke_probe(
            "run", self.provider.url("/backoff-cancel"), "2", "5", "1", env=env)
        self.assertEqual(proc.stdout.splitlines()[0], "RESULT 2 1 5",
                         proc.stdout + proc.stderr)
        self.assertIn("ERROR cancelled", proc.stdout)
        self.assertEqual(self.provider.hits["/backoff-cancel"], 1)

    def test_visible_delta_failure_never_retries_or_falls_back(self):
        case = self.root / "partial-deltas"
        case.mkdir()

        def event(field, text):
            return (b"data: " + compact({"choices": [{"delta": {
                field: text}}]}) + b"\n\n")

        for name, field in (("content", "content"),
                            ("reasoning", "reasoning_content")):
            with self.subTest(name=name):
                path = "/partial-" + name
                fallback = "/partial-fallback-" + name
                self.provider.add(path, [
                    (200, event(field, "visible-" + name),
                     "text/event-stream", True)])
                self.provider.add(fallback, [(200, chat("must not mix"))])
                proc = run_agent(case, {
                    "transport": "libcurl", "stream": True,
                    "show_thinking": True,
                    "providers": [
                        {"name": "partial", "model": "partial",
                         "base_url": self.provider.url(path)},
                        {"name": "fallback", "model": "fallback",
                         "base_url": self.provider.url(fallback)},
                    ],
                    "max_retries": 3, "retry_base_seconds": 0,
                })
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(self.provider.hits[path], 1, proc.stderr)
                self.assertEqual(self.provider.hits[fallback], 0, proc.stderr)
                self.assertNotIn("must not mix", proc.stdout + proc.stderr)
                self.assertIn("visible-" + name, proc.stdout + proc.stderr)
                self.assertIn("attempt 1/4", proc.stderr)

    def test_hidden_reasoning_failure_still_retries(self):
        case = self.root / "hidden-reasoning"
        case.mkdir()
        hidden = (b"data: " + compact({"choices": [{"delta": {
            "reasoning_content": "hidden-reasoning-sentinel"}}]}) + b"\n\n")
        self.provider.add("/hidden-reasoning", [
            (200, hidden, "text/event-stream", True),
            (200, sse_message("visible answer recovered"), "text/event-stream"),
        ])
        proc = run_agent(case, {
            "transport": "libcurl", "stream": True, "show_thinking": False,
            "base_url": self.provider.url("/hidden-reasoning"),
            "max_retries": 1, "retry_base_seconds": 0,
        })
        self.assertEqual((proc.returncode, proc.stdout),
                         (0, "visible answer recovered\n"), proc.stderr)
        self.assertEqual(self.provider.hits["/hidden-reasoning"], 2, proc.stderr)
        self.assertNotIn("hidden-reasoning-sentinel", proc.stdout)
        self.assertIn(hidden.decode(), proc.stderr)

    def test_strict_nofs_zero_delay_and_positive_delay_behavior(self):
        source = self.root / "provider_retry_nofs.c"
        binary = self.root / "provider-retry-nofs"
        source.write_text(NOFS_PROBE_SOURCE)
        subprocess.run([
            "gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DMOTIRIS_NO_FILESYSTEM", "-I" + str(ROOT / "include"),
            "-I" + str(ROOT / "src/vendor"), str(source),
            str(ROOT / "src/core/agent.c"), str(ROOT / "src/core/config.c"),
            str(ROOT / "src/vendor/cJSON.c"), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=30)
        proc = subprocess.run([str(binary)], cwd=ROOT, text=True,
                              capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 0, (proc.stdout, proc.stderr))
        self.assertIn("NOFS base=0 host=0 rc=0 hits=3 messages=2 delays=",
                      proc.stdout)
        self.assertIn("NOFS base=1 host=1 rc=0 hits=3 messages=2 delays=1,2",
                      proc.stdout)
        self.assertIn("nofs ok", proc.stdout)
        self.assertIn("NOFS base=1 host=0 rc=1 hits=1 messages=1 delays= "
                      "error=retry backoff unavailable without host I/O",
                      proc.stdout)

    def test_spawned_status_trailer_preserves_exact_body_budget(self):
        case = self.root / "spawned-boundary"
        case.mkdir()
        body = chat("spawned exact boundary")
        body += b" " * (512 - len(body))
        self.provider.add("/spawned-exact", [(200, body)])
        proc = run_agent(case, {
            "transport": "curl", "stream": False,
            "base_url": self.provider.url("/spawned-exact"),
        }, extra_env={"MOTIRIS_RESPONSE_MAX_BYTES": str(len(body))})
        self.assertEqual((proc.returncode, proc.stdout),
                         (0, "spawned exact boundary\n"), proc.stderr)
        self.assertEqual(self.provider.hits["/spawned-exact"], 1)

    def test_raw_non200_bodies_buffered_streamed_spawned_and_repl(self):
        case = self.root / "raw-errors"
        case.mkdir()
        json_body = (b'{\n  "error": {"message": "' + b"J" * 240 +
                     b'"}, "pad": [1, 2, 3]\n}\n')
        text_body = (b"first line %s literal\n" + b"P" * 230 +
                     b"\n\x1b[31mraw-control\x1b[0m\nlast line\n")
        for body_name, body in (("json", json_body), ("text", text_body)):
            for transport in ("libcurl", "curl"):
                for stream in (False, True):
                    name = "%s-%s-%s" % (body_name, transport, stream)
                    with self.subTest(name=name):
                        path = "/raw-" + name
                        self.provider.add(path, [(500, body)])
                        proc = run_agent(case, {
                            "transport": transport, "stream": stream,
                            "base_url": self.provider.url(path),
                            "max_retries": 1, "retry_base_seconds": 0,
                        })
                        self.assertNotEqual(proc.returncode, 0, proc.stdout)
                        self.assertEqual(self.provider.hits[path], 2)
                        self.assertEqual(proc.stderr.count(body.decode()), 2,
                                         proc.stderr)
                        self.assertIn("http status 500", proc.stderr)
                        self.assertIn("attempt 1/2", proc.stderr)
                        self.assertIn("attempt 2/2", proc.stderr)
                        self.assertNotIn("retry-test-key",
                                         proc.stdout + proc.stderr)

        self.provider.add("/raw-default", [(429, text_body)])
        proc = run_agent(case, {
            "transport": "libcurl", "stream": False,
            "base_url": self.provider.url("/raw-default"),
        })
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(self.provider.hits["/raw-default"], 1)
        self.assertEqual(proc.stderr.count(text_body.decode()), 1, proc.stderr)
        self.assertIn("attempt 1/1", proc.stderr)

        self.provider.add("/raw-repl", [(500, text_body)])
        proc = run_repl(case, {
            "transport": "libcurl", "stream": True,
            "base_url": self.provider.url("/raw-repl"),
        })
        self.assertEqual(proc.returncode, 0, proc.stdout)
        self.assertEqual(self.provider.hits["/raw-repl"], 1)
        self.assertEqual(proc.stdout.count(text_body.decode()), 1, proc.stdout)

        # Exercise the real async diagnostic callback, chunking and PTY renderer.
        from repl_cards import Repl, plain
        large_body = b"raw-pty-begin\n" + b"Q" * 9000 + b"\nraw-pty-end\n"
        self.provider.add("/raw-pty", [(500, large_body)])
        env = agent_environment(case, {
            "transport": "libcurl", "stream": True,
            "base_url": self.provider.url("/raw-pty"),
        })
        repl = Repl(env, command=[
            str(BINARY), "-i", "--no-tools", "--no-plugin", "--max-steps", "1",
        ])
        try:
            repl.read_until("motiris> ")
            repl.clear()
            repl.send("retry route\r")
            output = plain(repl.read_until("raw-pty-end", timeout=10))
            self.assertIn(large_body.decode().rstrip("\n"), output)
            self.assertEqual(self.provider.hits["/raw-pty"], 1)
            repl.read_until("motiris> ", timeout=10)
            repl.send("/exit\r")
            repl.read_until("bye")
        finally:
            repl.close()

        for name, body in (("api", json_body), ("malformed", text_body)):
            path = "/raw-200-" + name
            self.provider.add(path, [(200, body)])
            proc = run_agent(case, {
                "transport": "libcurl", "stream": False,
                "base_url": self.provider.url(path),
                "max_retries": 1, "retry_base_seconds": 0,
            })
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(proc.stderr.count(body.decode()), 2, proc.stderr)

    def test_config_apply_invalid_values_and_reload(self):
        case = self.root / "config-case"
        case.mkdir()
        env, path = self.config_env(case)
        proc = self.invoke_probe("config", env=env)
        self.assertEqual((proc.returncode, proc.stdout), (0, "0 1\n"),
                         (proc.stdout, proc.stderr))
        path.write_text('{"max_retries":7,"retry_base_seconds":13,'
                        '"unknown":{"preserve":true}}')
        proc = self.invoke_probe("config", env=env)
        self.assertEqual((proc.returncode, proc.stdout), (0, "7 13\n"),
                         (proc.stdout, proc.stderr))
        for invalid in ('{"max_retries":"3","retry_base_seconds":9}',
                        '{"max_retries":9,"retry_base_seconds":61}',
                        '{"max_retries":-1,"retry_base_seconds":-1}',
                        '{"max_retries":1.5,"retry_base_seconds":true}'):
            path.write_text(invalid)
            proc = self.invoke_probe("config-preserve", env=env)
            self.assertEqual((proc.returncode, proc.stdout), (0, "6 9\n"),
                             (invalid, proc.stdout, proc.stderr))
        path.write_text('{"max_retries":5,"retry_base_seconds":7,'
                        '"unknown":{"preserve":true}}')
        proc = self.invoke_probe("config-reload", env=env)
        self.assertEqual(proc.returncode, 0, (proc.stdout, proc.stderr))
        self.assertIn('"unknown"', path.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
