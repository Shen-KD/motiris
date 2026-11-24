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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef MOTIRIS_NO_FILESYSTEM
#include <curl/curl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#endif

typedef struct {
  MotirisCancelCallback *cb;
  void *ud;
} CancelCtx;

typedef struct {
  char *data;
  size_t len, cap, limit;
  CancelCtx *cancel;
  int failed;                 /* 1 = configured limit, 2 = allocation */
} Buf;

static int cancel_requested(const CancelCtx *cancel) {
  return cancel && cancel->cb && cancel->cb(cancel->ud);
}

#ifndef MOTIRIS_NO_FILESYSTEM

#define RESPONSE_MAX_BYTES (16u * 1024u * 1024u)
#define STREAM_MAX_TOOL_CALLS 256
#define STREAM_WIRE_OVERHEAD 4u

/* One configurable ceiling applies independently to each receive/aggregate
 * buffer. It is an allocation ceiling, never a preallocation. */
static size_t response_limit(void) {
  const char *s = getenv("MOTIRIS_RESPONSE_MAX_BYTES");
  if (!s || !*s) return RESPONSE_MAX_BYTES;
  char *end = NULL;
  unsigned long value = strtoul(s, &end, 10);
  if (!end || *end || end == s || !value || value > RESPONSE_MAX_BYTES)
    return RESPONSE_MAX_BYTES; /* validated and reported by motiris_new */
  return (size_t)value;
}

static const char *buffer_error(int failed) {
  if (failed == 1) return "response limit exceeded";
  if (failed == 2) return "response allocation failed";
  return "invalid stream response";
}

static int buf_append(Buf *b, const char *p, size_t n) {
  if (b->failed) return -1;
  if (b->len > b->limit || n > b->limit - b->len) {
    b->failed = 1;
    return -1;
  }
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t maximum = b->limit + 1;
    size_t cap = b->cap ? b->cap : 1024;
    if (cap > maximum) cap = maximum;
    while (cap < need) {
      if (cap > maximum / 2) { cap = maximum; break; }
      cap *= 2;
    }
    if (cap < need) cap = need;
    char *grown = realloc(b->data, cap);
    if (!grown) {
      b->failed = 2;
      return -1;
    }
    b->data = grown;
    b->cap = cap;
  }
  if (n) memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

/* libcurl invokes this even while a response is stalled. Returning nonzero
 * aborts the transfer; stream_write_cb also checks on every incoming chunk. */
static int curl_progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
  (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  return cancel_requested(ud) ? 1 : 0;
}
/* incremental stream sink: called with each SSE chunk while streaming */
typedef struct {
  void (*on_delta)(const char *text, void *ud);
  void *ud;
  void (*on_reasoning)(const char *text, void *ud);
  void *reasoning_ud;
  CancelCtx *cancel;
  Buf line;                       /* one SSE line, including bounded NUL */
  Buf content;                    /* concatenated delta.content */
  Buf raw;                        /* buffered JSON fallback, before first SSE */
  int saw_sse;
  size_t received, wire_limit;    /* bounded JSON/SSE framing as well */
  int failed;
  cJSON *tool_calls;              /* array of {id,type,function} */
  cJSON *usage;                   /* last-chunk usage, if provided */
} StreamCtx;

/* parse one "data: {json}" payload: extract delta and aggregate */
static int stream_handle_data(StreamCtx *s, const char *payload) {
  cJSON *j = cJSON_Parse(payload);
  if (!j) return 0;
  cJSON *use = cJSON_GetObjectItemCaseSensitive(j, "usage");
  if (cJSON_IsObject(use)) {
    cJSON_Delete(s->usage);
    s->usage = cJSON_Duplicate(use, 1);
  }
  cJSON *choice = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
  cJSON *delta = choice ? cJSON_GetObjectItemCaseSensitive(choice, "delta") : NULL;
  if (!delta) { cJSON_Delete(j); return 0; }

  const char *reasoning = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content"));
  if (!reasoning)
    reasoning = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(delta, "reasoning"));
  if (reasoning && *reasoning && s->on_reasoning)
    s->on_reasoning(reasoning, s->reasoning_ud);

  const char *dc = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(delta, "content"));
  if (dc && *dc) {
    if (buf_append(&s->content, dc, strlen(dc))) {
      s->failed = s->content.failed;
      cJSON_Delete(j);
      return -1;
    }
    if (s->on_delta) s->on_delta(dc, s->ud);
  }

  cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
  if (cJSON_IsArray(tcs)) {
    if (!s->tool_calls) s->tool_calls = cJSON_CreateArray();
    for (cJSON *tc = tcs->child; tc; tc = tc->next) {
      cJSON *index = cJSON_GetObjectItemCaseSensitive(tc, "index");
      double number = cJSON_GetNumberValue(index);
      if (!cJSON_IsNumber(index) || number != number ||
          number < 0 || number >= STREAM_MAX_TOOL_CALLS) {
        s->failed = 3;
        cJSON_Delete(j);
        return -1;
      }
      int idx = (int)number;
      if ((double)idx != number) {
        s->failed = 3;
        cJSON_Delete(j);
        return -1;
      }
      cJSON *slot = cJSON_GetArrayItem(s->tool_calls, idx);
      if (!slot) {
        if (cJSON_GetArraySize(s->tool_calls) >= STREAM_MAX_TOOL_CALLS) {
          s->failed = 1;
          cJSON_Delete(j);
          return -1;
        }
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
      if (cJSON_IsString(id) && id->valuestring) {
        cJSON *current_id = cJSON_GetObjectItemCaseSensitive(slot, "id");
        if (!current_id) current_id = cJSON_AddStringToObject(slot, "id", "");
        if (!current_id || !cJSON_SetValuestring(current_id, id->valuestring)) {
          s->failed = 2;
          cJSON_Delete(j);
          return -1;
        }
      }
      if (d_fn) {
        cJSON *dn = cJSON_GetObjectItemCaseSensitive(d_fn, "name");
        if (cJSON_IsString(dn) && dn->valuestring && *dn->valuestring &&
            !cJSON_SetValuestring(
                cJSON_GetObjectItemCaseSensitive(slot_fn, "name"),
                dn->valuestring)) {
          s->failed = 2;
          cJSON_Delete(j);
          return -1;
        }
        cJSON *da = cJSON_GetObjectItemCaseSensitive(d_fn, "arguments");
        if (cJSON_IsString(da) && da->valuestring) {
          cJSON *cur = cJSON_GetObjectItemCaseSensitive(slot_fn, "arguments");
          size_t cc = strlen(cur->valuestring), nn = strlen(da->valuestring);
          if (cc > s->content.limit || nn > s->content.limit - cc) {
            s->failed = 1;
            cJSON_Delete(j);
            return -1;
          }
          char *merged = malloc(cc + nn + 1);
          if (!merged) {
            s->failed = 2;
            cJSON_Delete(j);
            return -1;
          }
          memcpy(merged, cur->valuestring, cc);
          memcpy(merged + cc, da->valuestring, nn + 1);
          if (!cJSON_SetValuestring(cur, merged)) {
            free(merged);
            s->failed = 2;
            cJSON_Delete(j);
            return -1;
          }
          free(merged);
        }
      }
    }
  }
  cJSON_Delete(j);
  return 0;
}

static int stream_feed(StreamCtx *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\n') {
      const char *line = s->line.data ? s->line.data : "";
      if (!strncmp(line, "data:", 5)) {
        s->saw_sse = 1;
        const char *payload = line + 5;
        while (*payload == ' ') payload++;
        if (strcmp(payload, "[DONE]") && *payload &&
            stream_handle_data(s, payload))
          return -1;
      }
      s->line.len = 0;
      if (s->line.data) s->line.data[0] = '\0';
    } else if (buf_append(&s->line, &c, 1)) {
      s->failed = s->line.failed;
      return -1;
    }
  }
  return 0;
}

static size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
  Buf *b = ud;
  if ((sz && nm > SIZE_MAX / sz) || cancel_requested(b->cancel)) return 0;
  size_t n = sz * nm;
  if (buf_append(b, ptr, n)) return 0;
  return n;
}

static char *libcurl_send(const char *url, const char *auth,
                          const char *body, MotirisCancelCallback *cancel_cb,
                          void *cancel_ud, char **err) {
  CURL *c = curl_easy_init();
  if (!c) { if (err) *err = strdup("libcurl init failed"); return NULL; }

  struct curl_slist *hdrs = NULL;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  if (auth) hdrs = curl_slist_append(hdrs, auth);

  CancelCtx cancel = { cancel_cb, cancel_ud };
  Buf out = { .cancel = &cancel, .limit = response_limit() };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancel);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "motiris/0.1");

  CURLcode rc = curl_easy_perform(c);
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK) {
    if (err) {
      if (cancel_requested(&cancel)) {
        *err = strdup("cancelled");
      } else if (out.failed) {
        *err = strdup(buffer_error(out.failed));
      } else {
        size_t n = strlen(curl_easy_strerror(rc)) + 32;
        char *m = malloc(n);
        snprintf(m, n, "curl: %s", curl_easy_strerror(rc));
        *err = m;
      }
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
  if ((sz && nm > SIZE_MAX / sz) || cancel_requested(s->cancel)) return 0;
  size_t n = sz * nm;
  if (s->received > s->wire_limit || n > s->wire_limit - s->received) {
    s->failed = 1;
    return 0;
  }
  s->received += n;
  /* Some OpenAI-compatible endpoints ignore stream:true and return JSON.
   * Keep a bounded copy only until the first SSE record establishes framing. */
  if (!s->saw_sse && !s->raw.failed) (void)buf_append(&s->raw, ptr, n);
  if (stream_feed(s, ptr, n)) return 0;
  if (s->saw_sse && s->raw.data) {
    free(s->raw.data);
    s->raw.data = NULL;
    s->raw.len = 0;
  }
  return n;
}

/* streamed variant: SSE chunks -> aggregated message via StreamCtx */
static char *libcurl_send_stream(const char *url, const char *auth,
                                 const char *body,
                                 void (*on_delta)(const char *, void *),
                                 void *ud,
                                 void (*on_reasoning)(const char *, void *),
                                 void *reasoning_ud,
                                 MotirisCancelCallback *cancel_cb,
                                 void *cancel_ud, char **err) {
  CURL *c = curl_easy_init();
  if (!c) { if (err) *err = strdup("libcurl init failed"); return NULL; }

  struct curl_slist *hdrs = NULL;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
  if (auth) hdrs = curl_slist_append(hdrs, auth);

  CancelCtx cancel = { cancel_cb, cancel_ud };
  size_t limit = response_limit();
  StreamCtx s = {
    .on_delta = on_delta, .ud = ud,
    .on_reasoning = on_reasoning, .reasoning_ud = reasoning_ud,
    .cancel = &cancel,
    .line = { .limit = limit }, .content = { .limit = limit },
    .raw = { .limit = limit },
    .wire_limit = limit * STREAM_WIRE_OVERHEAD,
  };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancel);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "motiris/0.1");

  CURLcode rc = curl_easy_perform(c);
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK) {
    if (err) {
      if (cancel_requested(&cancel)) {
        *err = strdup("cancelled");
      } else if (s.failed) {
        *err = strdup(buffer_error(s.failed));
      } else {
        size_t n = strlen(curl_easy_strerror(rc)) + 32;
        char *m = malloc(n);
        snprintf(m, n, "curl: %s", curl_easy_strerror(rc));
        *err = m;
      }
    }
    free(s.content.data); cJSON_Delete(s.tool_calls); cJSON_Delete(s.usage);
    free(s.line.data); free(s.raw.data);
    return NULL;
  }
  if (http != 200) {
    if (err) {
      char *m = malloc(96);
      snprintf(m, 96, "http status %ld (stream)", http);
      *err = m;
    }
    free(s.content.data); cJSON_Delete(s.tool_calls); cJSON_Delete(s.usage);
    free(s.line.data); free(s.raw.data);
    return NULL;
  }

  if (!s.saw_sse) {
    free(s.content.data); free(s.line.data);
    cJSON_Delete(s.tool_calls); cJSON_Delete(s.usage);
    if (s.raw.failed) {
      free(s.raw.data);
      if (err) *err = strdup(buffer_error(s.raw.failed));
      return NULL;
    }
    if (!s.raw.data) s.raw.data = strdup("");
    return s.raw.data;
  }

  /* rebuild one complete chat message from the delta aggregates */
  cJSON *resp = cJSON_CreateObject();
  cJSON *choices = cJSON_CreateArray();
  cJSON *choice = cJSON_CreateObject();
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "role", "assistant");
  cJSON_AddStringToObject(msg, "content",
                          s.content.data ? s.content.data : "");
  int has_tool_calls = s.tool_calls && cJSON_GetArraySize(s.tool_calls) > 0;
  if (has_tool_calls)
    cJSON_AddItemToObject(msg, "tool_calls", s.tool_calls);
  else cJSON_Delete(s.tool_calls);
  cJSON_AddItemToObject(choice, "message", msg);
  cJSON_AddItemToObject(choice, "finish_reason",
      cJSON_CreateString(has_tool_calls ? "tool_calls" : "stop"));
  cJSON_AddItemToArray(choices, choice);
  cJSON_AddItemToObject(resp, "choices", choices);
  if (s.usage) cJSON_AddItemToObject(resp, "usage", s.usage);
  free(s.content.data);
  free(s.line.data);
  free(s.raw.data);
  char *printed = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  return printed;
}

/* ---------------- curl child backend ---------------- */
static const char *find_curl(void) {
  const char *c = getenv("MOTIRIS_CURL");
  return (c && *c) ? c : "curl";
}

static char *read_all(int fd, pid_t pid, CancelCtx *cancel, int *cancelled,
                      size_t limit, int *failed) {
  Buf out = { .limit = limit };
  char chunk[4096];
  for (;;) {
    if (!*cancelled && cancel_requested(cancel)) {
      *cancelled = 1;
      kill(pid, SIGKILL);  /* bounded cancellation; waitpid below reaps it */
    }
    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLHUP };
    int ready = poll(&pfd, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (!ready) continue;
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n > 0) {
      if (buf_append(&out, chunk, (size_t)n)) {
        kill(pid, SIGKILL);
        break;
      }
      continue;
    }
    if (!n) break;
    if (errno != EINTR) break;
  }
  if (!out.failed && buf_append(&out, "", 0)) kill(pid, SIGKILL);
  *failed = out.failed;
  return out.data;
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
                        const char *body, MotirisCancelCallback *cancel_cb,
                        void *cancel_ud, char **err) {
  CancelCtx cancel = { cancel_cb, cancel_ud };
  if (cancel_requested(&cancel)) {
    if (err) *err = strdup("cancelled");
    return NULL;
  }
  int inpipe[2], outpipe[2];
  if (pipe(inpipe)) {
    if (err) *err = strdup("curl pipe setup failed");
    return NULL;
  }
  if (pipe(outpipe)) {
    close(inpipe[0]); close(inpipe[1]);
    if (err) *err = strdup("curl pipe setup failed");
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(inpipe[0]); close(inpipe[1]);
    close(outpipe[0]); close(outpipe[1]);
    if (err) *err = strdup("curl fork failed");
    return NULL;
  }

  if (pid == 0) { /* child */
    dup2(inpipe[0], 0);
    dup2(outpipe[1], 1);
    close(inpipe[0]); close(inpipe[1]);
    close(outpipe[0]); close(outpipe[1]);
    /* build argv dynamically: no -H auth header when there is no key */
    const char *curl = find_curl();
    char *argv[18];
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

  int cancelled = 0;
  int read_failed = 0;
  char *out = read_all(outpipe[0], pid, &cancel, &cancelled,
                       response_limit(), &read_failed);
  close(outpipe[0]);

  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  if (cancelled || cancel_requested(&cancel)) {
    if (err) *err = strdup("cancelled");
    free(out);
    return NULL;
  }
  if (read_failed) {
    if (err) *err = strdup(buffer_error(read_failed));
    free(out);
    return NULL;
  }
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

#endif  /* MOTIRIS_NO_FILESYSTEM */

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
                             const char *auth, const char *body,
                             MotirisCancelCallback *cancel, void *cancel_ud,
                             char **err);
char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud,
                                    void (*on_reasoning)(const char *, void *),
                                    void *reasoning_ud,
                                    MotirisCancelCallback *cancel,
                                    void *cancel_ud, char **err) {
  if (err) *err = NULL;
#ifndef MOTIRIS_NO_FILESYSTEM
  if (!backend || !strcmp(backend, "auto") || !strcmp(backend, "libcurl"))
    return libcurl_send_stream(url, auth, body, on_delta, ud,
                               on_reasoning, reasoning_ud,
                               cancel, cancel_ud, err);
#else
  (void)on_delta;
  (void)ud;
  (void)on_reasoning;
  (void)reasoning_ud;
#endif
  /* Other backends are buffered-only. Do not ask the provider for SSE and
   * then try to parse it as one JSON response: negotiate stream off instead. */
  cJSON *request = cJSON_Parse(body);
  char *buffered = NULL;
  if (request) {
    cJSON_DeleteItemFromObjectCaseSensitive(request, "stream");
    buffered = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
  }
  if (!buffered) {
    if (err) {
      static const char message[] = "buffered transport request allocation failed";
      *err = malloc(sizeof message);
      if (*err) memcpy(*err, message, sizeof message);
    }
    return NULL;
  }
  char *response = motiris_transport_send(backend, url, auth, buffered,
                                          cancel, cancel_ud, err);
  free(buffered);
  return response;
}

static char *transport_error(const char *msg) {
  size_t n = strlen(msg) + 1;
  char *copy = malloc(n);
  if (copy) memcpy(copy, msg, n);
  return copy;
}

char *motiris_transport_send(const char *backend, const char *url,
                             const char *auth, const char *body,
                             MotirisCancelCallback *cancel_cb, void *cancel_ud,
                             char **err) {
  if (err) *err = NULL;
  CancelCtx initial_cancel = { cancel_cb, cancel_ud };
  if (cancel_requested(&initial_cancel)) {
    if (err) *err = transport_error("cancelled");
    return NULL;
  }
#ifndef MOTIRIS_NO_FILESYSTEM
  if (!backend || !strcmp(backend, "auto") || !strcmp(backend, "libcurl"))
    return libcurl_send(url, auth, body, cancel_cb, cancel_ud, err);
  if (!strcmp(backend, "curl"))
    return spawn_curl(url, auth, body, cancel_cb, cancel_ud, err);
#else
  (void)url;
  (void)auth;
  (void)cancel_cb;
  (void)cancel_ud;
#endif
  if (backend && !strcmp(backend, "echo")) {
    cJSON *j = cJSON_Parse(body);
    if (!j) { if (err) *err = transport_error("bad request json"); return NULL; }
    char *pretty = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    char *r = echo_response(pretty);
    free(pretty);
    return r;
  }
#ifdef MOTIRIS_NO_FILESYSTEM
  if (err) *err = transport_error("transport unavailable without host I/O");
#else
  if (err) *err = transport_error("unknown transport backend");
#endif
  return NULL;
}
