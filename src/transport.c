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

/* incremental stream sink: called with each SSE chunk while streaming */
typedef struct {
  void (*on_delta)(const char *text, void *ud);
  void *ud;
  Buf buf;              /* last complete JSON object (for parsing) */
  char *line; size_t lcap, llen;   /* line buffer for SSE framing */
  /* aggregated final message (SSE deltas -> one complete message) */
  char *content;                    /* concatenated delta.content */
  cJSON *tool_calls;                /* array of {id,type,function} */
} StreamCtx;

/* parse one "data: {json}" payload: extract delta and aggregate */
static void stream_handle_data(StreamCtx *s, const char *payload) {
  cJSON *j = cJSON_Parse(payload);
  if (!j) return;
  cJSON *choice = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
  cJSON *delta = choice ? cJSON_GetObjectItemCaseSensitive(choice, "delta") : NULL;
  if (!delta) { cJSON_Delete(j); return; }

  const char *dc = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(delta, "content"));
  if (dc && *dc) {
    size_t n = strlen(dc);
    s->content = realloc(s->content,
        (s->content ? strlen(s->content) : 0) + n + 1);
    strcat(s->content ? s->content : (s->content = malloc(1), s->content[0] = 0, s->content), dc);
    if (s->on_delta) s->on_delta(dc, s->ud);
  }

  cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
  if (cJSON_IsArray(tcs)) {
    if (!s->tool_calls) s->tool_calls = cJSON_CreateArray();
    for (cJSON *tc = tcs->child; tc; tc = tc->next) {
      int idx = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive(tc, "index"));
      cJSON *slot = cJSON_GetArrayItem(s->tool_calls, idx);
      if (!slot) {
        slot = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddItemToObject(fn, "name", cJSON_CreateString(""));
        cJSON_AddItemToObject(fn, "arguments", cJSON_CreateString(""));
        cJSON_AddItemToObject(slot, "function", fn);
        cJSON_AddItemToObject(slot, "type", cJSON_CreateString("function"));
        cJSON_AddItemToArray(s->tool_calls, slot);
      }
      cJSON *d_fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
      cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
      cJSON *slot_fn = cJSON_GetObjectItemCaseSensitive(slot, "function");
      if (cJSON_IsString(id) && id->valuestring)
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(slot, "id")
            ? cJSON_GetObjectItemCaseSensitive(slot, "id")
            : cJSON_AddStringToObject(slot, "id", ""), id->valuestring);
      if (d_fn) {
        cJSON *dn = cJSON_GetObjectItemCaseSensitive(d_fn, "name");
        if (cJSON_IsString(dn) && dn->valuestring && *dn->valuestring)
          cJSON_SetValuestring(
              cJSON_GetObjectItemCaseSensitive(slot_fn, "name"),
              dn->valuestring);
        cJSON *da = cJSON_GetObjectItemCaseSensitive(d_fn, "arguments");
        if (cJSON_IsString(da) && da->valuestring) {
          cJSON *cur = cJSON_GetObjectItemCaseSensitive(slot_fn, "arguments");
          size_t cc = strlen(cur->valuestring), nn = strlen(da->valuestring);
          char *merged = malloc(cc + nn + 1);
          memcpy(merged, cur->valuestring, cc);
          memcpy(merged + cc, da->valuestring, nn + 1);
          cJSON_SetValuestring(cur, merged);
          free(merged);
        }
      }
    }
  }
  cJSON_Delete(j);
}

static void stream_feed(StreamCtx *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\n') {
      s->line[s->llen] = '\0';
      if (!strncmp(s->line, "data:", 5)) {
        char *payload = s->line + 5;
        while (*payload == ' ') payload++;
        if (strcmp(payload, "[DONE]") && *payload)
          stream_handle_data(s, payload);
      }
      s->llen = 0;
    } else {
      if (s->llen + 2 >= s->lcap) { s->lcap = s->lcap ? s->lcap * 2 : 1024; s->line = realloc(s->line, s->lcap); }
      s->line[s->llen++] = c;
    }
  }
}

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

/* curl write-callback adapter: feed raw bytes into the SSE line buffer */
static size_t stream_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
  StreamCtx *s = ud;
  stream_feed(s, ptr, sz * nm);
  return sz * nm;
}

/* streamed variant: SSE chunks -> aggregated message via StreamCtx */
static char *libcurl_send_stream(const char *url, const char *auth,
                                 const char *body,
                                 void (*on_delta)(const char *, void *),
                                 void *ud, char **err) {
  CURL *c = curl_easy_init();
  if (!c) { if (err) *err = strdup("libcurl init failed"); return NULL; }

  struct curl_slist *hdrs = NULL;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
  if (auth) hdrs = curl_slist_append(hdrs, auth);

  StreamCtx s = { .on_delta = on_delta, .ud = ud };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
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
    free(s.content); cJSON_Delete(s.tool_calls); free(s.line);
    return NULL;
  }
  if (http != 200) {
    if (err) {
      char *m = malloc(96);
      snprintf(m, 96, "http status %ld (stream)", http);
      *err = m;
    }
    free(s.content); cJSON_Delete(s.tool_calls); free(s.line);
    return NULL;
  }

  /* rebuild one complete chat message from the delta aggregates */
  cJSON *resp = cJSON_CreateObject();
  cJSON *choices = cJSON_CreateArray();
  cJSON *choice = cJSON_CreateObject();
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "role", "assistant");
  cJSON_AddStringToObject(msg, "content", s.content ? s.content : "");
  if (s.tool_calls && cJSON_GetArraySize(s.tool_calls) > 0)
    cJSON_AddItemToObject(msg, "tool_calls", s.tool_calls);
  else cJSON_Delete(s.tool_calls);
  cJSON_AddItemToObject(choice, "message", msg);
  cJSON_AddItemToObject(choice, "finish_reason",
      cJSON_CreateString(s.tool_calls ? "tool_calls" : "stop"));
  cJSON_AddItemToArray(choices, choice);
  cJSON_AddItemToObject(resp, "choices", choices);
  free(s.content);
  free(s.line);
  return cJSON_PrintUnformatted(resp);
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
                             const char *auth, const char *body, char **err);
char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud, char **err) {
  if (err) *err = NULL;
  if (!backend || !strcmp(backend, "auto") || !strcmp(backend, "libcurl"))
    return libcurl_send_stream(url, auth, body, on_delta, ud, err);
  /* other backends: fall back to buffered send (aggregation identical) */
  return motiris_transport_send(backend, url, auth, body, err);
}

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