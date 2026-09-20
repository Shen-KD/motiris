/* transport.c - send a chat request, get the raw response body.
 *
 * Backends:
 *   curl   - spawn the system `curl` binary (default). Zero link-time
 *            dependencies; HTTPS/TLS handled by curl itself. The only
 *            external piece is a curl(1) on PATH, and it is only alive
 *            while a request is in flight - the agent itself stays tiny.
 *   echo   - no network. Returns a canned OpenAI-style response that
 *            exercises the tool-calling loop (great for smoke tests and
 *            dry runs without an API key).
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---------------- curl child backend ---------------- */
static const char *find_curl(void) {
  const char *c = getenv("MOTIRIS_CURL");
  return (c && *c) ? c : "curl";
}

static char *read_all(int fd) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  ssize_t n;
  while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
    len += (size_t)n;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  buf[len] = '\0';
  return buf;
}

static int write_all(int fd, const char *s) {
  size_t len = strlen(s), off = 0;
  while (off < len) {
    ssize_t n = write(fd, s + off, len - off);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static char *spawn_curl(const char *url, const char *auth,
                        const char *body, char **err) {
  int inpipe[2], outpipe[2];
  if (pipe(inpipe) || pipe(outpipe)) return NULL;

  pid_t pid = fork();
  if (pid < 0) return NULL;

  if (pid == 0) { /* child */
    dup2(inpipe[0], 0);
    dup2(outpipe[1], 1);
    close(inpipe[0]); close(inpipe[1]);
    close(outpipe[0]); close(outpipe[1]);
    const char *curl = find_curl();
    execlp(curl, curl, "-sS", "-L", "--max-time", "120",
           "-X", "POST", url,
           "-H", "Content-Type: application/json",
           "-H", "Accept: application/json",
           auth ? "-H" : "", auth ? auth : "",
           "--data-binary", "@-",
           (char *)NULL);
    _exit(127);
  }

  close(inpipe[0]);
  close(outpipe[1]);
  int wr = write_all(inpipe[1], body);
  close(inpipe[1]);

  char *out = read_all(outpipe[0]);
  close(outpipe[0]);

  int st = 0;
  waitpid(pid, &st, 0);
  if (wr < 0) { free(out); return NULL; }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !*out) {
    if (err) {
      size_t need = 64 + strlen(out ? out : "");
      char *m = malloc(need);
      snprintf(m, need, "curl exited %d: %s",
               WIFEXITED(st) ? WEXITSTATUS(st) : -1, out ? out : "");
      *err = m;
    }
    free(out);
    return NULL;
  }
  return out;
}

/* ---------------- echo backend ---------------- */
static char *echo_response(const char *body) {
  /* Round 1 (no tool result yet): ask for a shell tool call so the whole
   * loop is exercised. Round 2 (tool result present): final answer. */
  int tool_round = strstr(body, "\"role\":\"tool\"") != NULL;

  cJSON *resp = cJSON_CreateObject();
  cJSON *choices = cJSON_CreateArray();
  cJSON *choice = cJSON_CreateObject();
  cJSON *msg = cJSON_CreateObject();

  cJSON_AddStringToObject(msg, "role", "assistant");
  if (!tool_round) {
    cJSON *calls = cJSON_CreateArray();
    cJSON *tc = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "shell");
    cJSON_AddStringToObject(fn, "arguments",
                            "{\"command\":\"echo 'motiris works'\"}");
    cJSON_AddItemToObject(tc, "function", fn);
    cJSON_AddItemToObject(tc, "id", cJSON_CreateString("call_echo_1"));
    cJSON_AddItemToObject(tc, "type", cJSON_CreateString("function"));
    cJSON_AddItemToArray(calls, tc);
    cJSON_AddItemToObject(msg, "tool_calls", calls);
    cJSON_AddItemToObject(choice, "finish_reason",
                          cJSON_CreateString("tool_calls"));
  } else {
    cJSON_AddStringToObject(msg, "content", "echo round complete: [ok]");
    cJSON_AddItemToObject(choice, "finish_reason",
                          cJSON_CreateString("stop"));
  }
  cJSON_AddItemToObject(choice, "message", msg);
  cJSON_AddItemToArray(choices, choice);
  cJSON_AddItemToObject(resp, "choices", choices);
  cJSON_AddStringToObject(resp, "id", "chatcmpl-echo");
  cJSON_AddStringToObject(resp, "object", "chat.completion");

  char *s = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  return s;
}

/* ---------------- dispatch ---------------- */
char *motiris_transport_send(const char *backend, const char *url,
                          const char *auth, const char *body, char **err) {
  if (err) *err = NULL;
  if (!backend || !strcmp(backend, "curl"))
    return spawn_curl(url, auth, body, err);
  if (!strcmp(backend, "echo")) {
    cJSON *j = cJSON_Parse(body);
    if (!j) { if (err) *err = strdup("bad request json"); return NULL; }
    char *pretty = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    char *r = echo_response(pretty);
    free(pretty);
    return r;
  }
  if (err) *err = strdup("unknown transport backend");
  return NULL;
}