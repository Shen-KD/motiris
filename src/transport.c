/* transport.c - send a chat request, get the raw response body.
 *
 * Backends:
 *   libcurl - built-in C library backend (default). No child process:
 *             curl handles HTTPS/TLS inside the agent, tied to the
 *             libcurl shared library at runtime.
 *   curl    - fallback: spawn the system `curl` binary (for hosts
 *             without the libcurl dev headers at build time).
 *   echo    - no network. Returns a canned OpenAI-style response that
 *             exercises the tool-calling loop (smoke tests, dry runs).
 *
 * Default backend is "auto": use libcurl when available, else spawn.
 */
#include "motiris.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct { char *data; size_t len; } Buf;

static size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
  Buf *b = ud;
  size_t n = sz * nm;
  b->data = realloc(b->data, b->len + n + 1);
  memcpy(b->data + b->len, ptr, n);
  b->len += n;
  b->data[b->len] = '\0';
  return n;
}

static char *libcurl_send(const char *url, const char *auth,
                          const char *body, char **err) {
  CURL *c = curl_easy_init();
  if (!c) { if (err) *err = strdup("libcurl init failed"); return NULL; }

  struct curl_slist *hdrs = NULL;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  if (auth) hdrs = curl_slist_append(hdrs, auth);

  Buf out = {0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "motiris/0.1");

  CURLcode rc = curl_easy_perform(c);
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK) {
    if (err) {
      size_t n = strlen(curl_easy_strerror(rc)) + 32;
      char *m = malloc(n);
      snprintf(m, n, "curl: %s", curl_easy_strerror(rc));
      *err = m;
    }
    free(out.data);
    return NULL;
  }
  if (http != 200) {
    if (err) {
      size_t n = 128 + (out.data ? strlen(out.data) : 0);
      char *m = malloc(n);
      snprintf(m, n, "http status %ld: %.200s", http,
               out.data ? out.data : "");
      *err = m;
    }
    free(out.data);
    return NULL;
  }
  return out.data;
}

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
    /* build argv dynamically: no -H auth header when there is no key */
    const char *curl = find_curl();
    char *argv[16];
    int ai = 0;
    argv[ai++] = (char *)curl;
    argv[ai++] = "-sS"; argv[ai++] = "-L";
    argv[ai++] = "--max-time"; argv[ai++] = "120";
    argv[ai++] = "-X"; argv[ai++] = "POST"; argv[ai++] = (char *)url;
    argv[ai++] = "-H"; argv[ai++] = "Content-Type: application/json";
    argv[ai++] = "-H"; argv[ai++] = "Accept: application/json";
    if (auth) { argv[ai++] = "-H"; argv[ai++] = (char *)auth; }
    argv[ai++] = "--data-binary"; argv[ai++] = "@-";
    argv[ai] = NULL;
    execvp(curl, argv);
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
  if (!backend || !strcmp(backend, "auto") || !strcmp(backend, "libcurl"))
    return libcurl_send(url, auth, body, err);
  if (!strcmp(backend, "curl"))
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