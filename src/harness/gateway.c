/* gateway.c - embedded HTTP gateway for motiris.
 *
 * Turns motiris into a resident service (like a message-platform
 * gateway, minus the platform): each chat_id gets its own agent
 * instance (its own context), so many conversations can run on one
 * process. Request handling is sequential (single-threaded accept loop
 * with bounded nonblocking connection I/O) - fine for modest traffic.
 *
 *   GET  /health                 -> {"status":"ok"}
 *   POST /v1/chat                -> {"reply":"..."}
 *        body: {"chat_id":"...","message":"...","reset":true?}
 *        auth: Authorization: Bearer <token> (when token set)
 *
 * Sessions are bounded full-turn JSON snapshots under
 * ~/.local/share/motiris/gateway/<hex(chat_id)>.log.  The snapshot binds
 * the encoded chat_id and restores complete messages only after ownership,
 * integrity and tool-call/result validation.
 */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SESSIONS 64
#define MAX_CHAT_ID_BYTES 120
#define MAX_CHAT_ID_ENCODED (MAX_CHAT_ID_BYTES * 2)
#define MAX_HEADER (16 * 1024)
#define MAX_BODY (1 << 20)
#define GATEWAY_DEFAULT_TIMEOUT_S 30L
#define GATEWAY_MAX_TIMEOUT_S 120L

/* ---------------- session table ---------------- */
typedef struct Session {
  char *chat_id;
  MotirisAgent *agent;
  char *log_path;
  char *session_id;
  struct Session *next;
} Session;

static Session *sessions;
static int n_sessions;
static char *gw_token;
static int gw_verbose;
/* Process-wide immutable configuration, never per-Agent/session state. */
static long gw_io_timeout_ms = GATEWAY_DEFAULT_TIMEOUT_S * 1000L;

static void session_dispose(Session *s) {
  if (!s) return;
  motiris_free(s->agent);
  free(s->chat_id);
  free(s->log_path);
  free(s->session_id);
  free(s);
}

static Session *find_session(const char *chat_id) {
  for (Session *s = sessions; s; s = s->next)
    if (!strcmp(s->chat_id, chat_id)) return s;
  return NULL;
}

static void evict_oldest(void) {
  if (!sessions) return;
  Session *p = sessions, *prev = NULL;
  while (p && p->next) { prev = p; p = p->next; }
  if (prev) prev->next = NULL; else sessions = NULL;
  session_dispose(p);
  n_sessions--;
}

/* Lowercase hex is injective over the complete chat_id byte string and
 * stays within NAME_MAX: 120 input bytes -> 240 chars + ".log". */
static void encode_chat_id(char out[MAX_CHAT_ID_ENCODED + 1], const char *id) {
  static const char hex[] = "0123456789abcdef";
  size_t j = 0;
  for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
    out[j++] = hex[*p >> 4];
    out[j++] = hex[*p & 15];
  }
  out[j] = '\0';
}

#ifndef MOTIRIS_NO_PERSISTENCE
static char *session_log_path(const char *chat_id) {
  const char *h = getenv("HOME");
  if (!h) return NULL;
  char encoded[MAX_CHAT_ID_ENCODED + 1];
  encode_chat_id(encoded, chat_id);
  size_t n = strlen(h) + strlen(encoded) + 48;
  char *path = malloc(n);
  if (path)
    snprintf(path, n, "%s/.local/share/motiris/gateway/%s.log", h, encoded);
  return path;
}

static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static int load_session_log(Session *s, const char **error) {
  struct stat st;
  if (stat(s->log_path, &st)) {
    if (errno == ENOENT) return 0; /* first turn creates it atomically */
    *error = "cannot stat session snapshot";
    return -1;
  }
  char encoded[MAX_CHAT_ID_ENCODED + 1];
  encode_chat_id(encoded, s->chat_id);
  size_t loaded = 0;
  if (motiris_session_load(s->agent, s->log_path, encoded, &loaded, error))
    return -1;
  return 0;
}
#endif

static Session *get_session(const char *chat_id, int tools_on,
                            const char **error) {
  Session *s = find_session(chat_id);
  if (s) return s;
  *error = "could not create session";

  s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->chat_id = strdup(chat_id);
  s->agent = motiris_new();
  s->session_id = motiris_session_auto_id();
  if (!s->chat_id || !s->agent) goto fail;
  if (!s->session_id) { session_dispose(s); return NULL; }
  if (!tools_on) motiris_set_tools_enabled(s->agent, 0);
  motiris_set_print_answer(s->agent, 0);
  motiris_apply_config(s->agent);   /* model/base_url/transport from config */
  if (tools_on) motiris_register_core_tools(s->agent);
  motiris_register_default_skill_tools(s->agent, NULL);
  motiris_append_skill_context(s->agent);

#ifndef MOTIRIS_NO_PERSISTENCE
  int restored_auto = 0;
  char *restored_id = NULL;
  int restore_rc = motiris_session_auto_restore(
      s->agent, "gateway", chat_id, NULL, &restored_id, NULL, error);
  if (restore_rc < 0) { session_dispose(s); return NULL; }
  if (!restore_rc) {
    free(s->session_id);
    s->session_id = restored_id;
    restored_auto = 1;
  }
  /* Per-chat legacy log remains as a migration fallback and dual-write
   * compatibility path; new automatic traces are authoritative when present. */
  if (getenv("HOME")) {
    s->log_path = session_log_path(chat_id);
    if (!s->log_path) goto fail;
    char *dir = strdup(s->log_path);
    if (!dir) goto fail;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    mkdir_p(dir);
    free(dir);
    if (!restored_auto && load_session_log(s, error)) {
      session_dispose(s);
      return NULL;
    }
  }
#endif
  if (n_sessions >= MAX_SESSIONS) evict_oldest();
  s->next = sessions;
  sessions = s;
  n_sessions++;
  return s;

fail:
  session_dispose(s);
  return NULL;
}

/* ---------------- HTTP plumbing ---------------- */
/* Return one absolute deadline. Each request receive and each complete
 * response send gets its own budget; progress and EINTR never extend it. */
static int gateway_deadline(struct timespec *deadline) {
  if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return -1;
  deadline->tv_sec += gw_io_timeout_ms / 1000L;
  long ns = deadline->tv_nsec + (gw_io_timeout_ms % 1000L) * 1000000L;
  deadline->tv_sec += ns / 1000000000L;
  deadline->tv_nsec = ns % 1000000000L;
  return 0;
}

/* 1 = ready, 0 = monotonic deadline expired, -1 = clock/poll failure. */
static int gateway_wait(int fd, short events,
                        const struct timespec *deadline) {
  for (;;) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    time_t sec = deadline->tv_sec - now.tv_sec;
    long nsec = deadline->tv_nsec - now.tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    if (sec < 0 || (sec == 0 && nsec <= 0)) {
      errno = ETIMEDOUT;
      return 0;
    }
    /* The configured maximum is 120 seconds, so this conversion is bounded. */
    int wait_ms = (int)(sec * 1000L + (nsec + 999999L) / 1000000L);
    struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
    int ready = poll(&pfd, 1, wait_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (!ready) {
      errno = ETIMEDOUT;
      return 0;
    }
    if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
    /* HUP/ERR are returned too: the following read/write obtains EOF/error. */
    return 1;
  }
}

static int gateway_write_all(int fd, const char *buf, size_t len,
                             const struct timespec *deadline) {
  size_t offset = 0;
  while (offset < len) {
    if (gateway_wait(fd, POLLOUT, deadline) <= 0) return -1;
    ssize_t written = write(fd, buf + offset, len - offset);
    if (written > 0) { offset += (size_t)written; continue; }
    if (written < 0 && (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK)) continue;
    if (!written) errno = EPIPE;
    return -1;
  }
  return 0;
}

static void send_json(int fd, int code, cJSON *body) {
  char *s = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  char hdr[256];
  const char *reason = code == 200 ? "OK" :
                       code == 401 ? "Unauthorized" :
                       code == 404 ? "Not Found" :
                       code == 408 ? "Request Timeout" :
                       code == 413 ? "Payload Too Large" :
                       code == 500 ? "Internal Server Error" : "Bad Request";
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
           "Content-Length: %zu\r\nConnection: close\r\n\r\n",
           code, reason, strlen(s));
  struct timespec deadline;
  if (gateway_deadline(&deadline) != 0 ||
      gateway_write_all(fd, hdr, strlen(hdr), &deadline) != 0 ||
      gateway_write_all(fd, s, strlen(s), &deadline) != 0) {
    if (errno == ETIMEDOUT)
      fprintf(stderr, "motiris gateway: response write timed out\n");
    else if (gw_verbose)
      fprintf(stderr, "motiris gateway: response write failed: %s\n",
              strerror(errno));
  }
  free(s);
}

static void send_err(int fd, int code, const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  send_json(fd, code, e);
}

typedef struct { int fd, failed; } SseWriter;

static void sse_write_failed(SseWriter *writer) {
  if (!writer->failed) {
    if (errno == ETIMEDOUT)
      fprintf(stderr, "motiris gateway: response write timed out\n");
    else if (gw_verbose)
      fprintf(stderr, "motiris gateway: SSE response write failed: %s\n",
              strerror(errno));
  }
  writer->failed = 1;
}

static void send_sse_event(SseWriter *writer, const char *event, cJSON *body) {
  char *json = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (writer->failed || !json) { free(json); sse_write_failed(writer); return; }
  const char prefix[] = "event: ";
  const char middle[] = "\ndata: ";
  const char suffix[] = "\n\n";
  struct timespec deadline;
  if (gateway_deadline(&deadline) ||
      gateway_write_all(writer->fd, prefix, sizeof prefix - 1, &deadline) ||
      gateway_write_all(writer->fd, event, strlen(event), &deadline) ||
      gateway_write_all(writer->fd, middle, sizeof middle - 1, &deadline) ||
      gateway_write_all(writer->fd, json, strlen(json), &deadline) ||
      gateway_write_all(writer->fd, suffix, sizeof suffix - 1, &deadline))
    sse_write_failed(writer);
  free(json);
}

static int send_sse_start(SseWriter *writer, int fd) {
  static const char headers[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  struct timespec deadline;
  writer->fd = fd;
  if (gateway_deadline(&deadline) ||
      gateway_write_all(fd, headers, sizeof headers - 1, &deadline))
    sse_write_failed(writer);
  return writer->failed ? -1 : 0;
}

static void gateway_delta(const char *text, void *ud) {
  SseWriter *writer = ud;
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "delta", text);
  send_sse_event(writer, "content", body);
}

static void gateway_reasoning(const char *text, void *ud) {
  SseWriter *writer = ud;
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "delta", text);
  send_sse_event(writer, "reasoning", body);
}

/* Find the end of the header block without reading past received bytes. */
static size_t find_header_end(const char *buf, size_t len, size_t start) {
  if (len < 4) return 0;
  for (size_t i = start; i <= len - 4; i++)
    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
        buf[i + 2] == '\r' && buf[i + 3] == '\n')
      return i + 4;
  return 0;
}

static int is_content_length(const char *line, size_t len) {
  static const char name[] = "content-length:";
  if (len < sizeof name - 1) return 0;
  for (size_t i = 0; i < sizeof name - 1; i++) {
    char c = line[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    if (c != name[i]) return 0;
  }
  return 1;
}

/* 0 = valid, -1 = malformed, 1 = body exceeds MAX_BODY. */
static int parse_content_length(const char *req, size_t header_len,
                                size_t *body_len) {
  size_t pos = 0;
  int seen = 0;
  *body_len = 0;
  while (pos < header_len) {
    size_t end = pos;
    while (end + 1 < header_len &&
           !(req[end] == '\r' && req[end + 1] == '\n')) end++;
    if (end + 1 >= header_len) return -1;
    if (is_content_length(req + pos, end - pos)) {
      size_t i = pos + 15, value = 0;
      if (seen) return -1;
      seen = 1;
      while (i < end && (req[i] == ' ' || req[i] == '\t')) i++;
      if (i == end || req[i] < '0' || req[i] > '9') return -1;
      for (; i < end && req[i] >= '0' && req[i] <= '9'; i++) {
        unsigned int digit = (unsigned int)(req[i] - '0');
        if (value > (MAX_BODY - digit) / 10) return 1;
        value = value * 10 + digit;
      }
      while (i < end && (req[i] == ' ' || req[i] == '\t')) i++;
      if (i != end) return -1;
      *body_len = value;
    }
    pos = end + 2;
  }
  return 0;
}

static int json_hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* cJSON exposes decoded strings as NUL-terminated C strings without a
 * separate length, and its invalid hex-digit conversion can also yield NUL.
 * Validate every Unicode escape and reject raw/decoded NUL before parsing so
 * IDs/messages cannot silently acquire a hidden suffix. Return 1 for NUL, 2
 * for malformed Unicode. The scan is O(n), allocation-free and distinguishes
 * a double-escaped literal from an actual Unicode escape. */
static int json_string_error(const char *json, size_t length) {
  int in_string = 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)json[i];
    if (!c) return 1;
    if (!in_string) {
      if (c == '"') in_string = 1;
      continue;
    }
    if (c == '"') { in_string = 0; continue; }
    if (c != '\\' || i + 1 >= length) continue;
    c = (unsigned char)json[++i];
    if (c != 'u') continue;
    if (length - i < 5) return 2;
    unsigned int codepoint = 0;
    for (size_t j = 1; j <= 4; j++) {
      int digit = json_hex_digit((unsigned char)json[i + j]);
      if (digit < 0) return 2;
      codepoint = (codepoint << 4) | (unsigned int)digit;
    }
    if (!codepoint) return 1;
    i += 4;
  }
  return 0;
}

/* Read one bounded request. The allocation is at most
 * MAX_HEADER + MAX_BODY + 1 bytes, including the trailing NUL. */
static char *read_request(int fd, int *status, size_t *request_body_len) {
  size_t cap = 8192, len = 0, header_len = 0, body_len = 0, searched = 0;
  struct timespec deadline;
  *status = 500;
  *request_body_len = 0;
  if (gateway_deadline(&deadline) != 0) return NULL;
  char *buf = malloc(cap + 1);
  if (!buf) return NULL;
  for (;;) {
    if (len == cap) {
      size_t limit = header_len ? header_len + body_len : MAX_HEADER;
      if (cap >= limit) { *status = 413; goto fail; }
      size_t next = cap > limit / 2 ? limit : cap * 2;
      char *tmp = realloc(buf, next + 1);
      if (!tmp) { *status = 500; goto fail; }
      buf = tmp;
      cap = next;
    }
    int ready = gateway_wait(fd, POLLIN, &deadline);
    if (ready <= 0) { *status = ready == 0 ? 408 : 500; goto fail; }
    ssize_t n = read(fd, buf + len, cap - len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      *status = 400;
      goto fail;
    }
    if (n == 0) { *status = 400; goto fail; }
    len += (size_t)n;
    if (!header_len) {
      /* Recheck only the three-byte overlap and newly received bytes. */
      size_t start = searched > 3 ? searched - 3 : 0;
      header_len = find_header_end(buf, len, start);
      searched = len;
      if (header_len) {
        int cl = parse_content_length(buf, header_len, &body_len);
        if (cl < 0) { *status = 400; goto fail; }
        if (cl > 0) { *status = 413; goto fail; }
      } else if (len == MAX_HEADER) {
        *status = 413;
        goto fail;
      }
    }
    if (header_len && len - header_len >= body_len) {
      len = header_len + body_len; /* ignore pipelined/extra bytes */
      buf[len] = '\0';
      *status = 0;
      *request_body_len = body_len;
      return buf;
    }
  }
fail:
  free(buf);
  return NULL;
}

static const char *header_of(const char *req, const char *name) {
  size_t nl = strlen(name);
  const char *p = req;
  while ((p = strstr(p, name)) != NULL) {
    if (p == req || p[-1] == '\n') {
      const char *v = p + nl;
      while (*v == ' ' || *v == '\t') v++;
      const char *e = v;
      while (*e && *e != '\r' && *e != '\n') e++;
      static char val[1024];
      size_t n = (size_t)(e - v);
      if (n >= sizeof val) n = sizeof val - 1;
      memcpy(val, v, n);
      val[n] = '\0';
      return val;
    }
    p += nl;
  }
  return NULL;
}

static int check_token(const char *req, int *bad) {
  if (!gw_token) return 1;
  const char *auth = header_of(req, "Authorization:");
  if (auth && !strncmp(auth, "Bearer ", 7) && !strcmp(auth + 7, gw_token))
    return 1;
  *bad = 1;
  return 0;
}

static int gateway_goal_prompt(const cJSON *message) {
  const char *role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "role"));
  const char *prompt = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "content"));
  return role && !strcmp(role, "user") && prompt &&
         !strncmp(prompt, "Goal check: <", 13);
}

static int gateway_done_reply(const char *s) {
  while (*s && strchr(" \t\r\n.", *s)) s++;
  size_t n = strlen(s);
  while (n && strchr(" \t\r\n.", s[n - 1])) n--;
  if (n != 4) return 0;
  for (size_t i = 0; i < 4; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != "done"[i]) return 0;
  }
  return 1;
}

/* Return the current run's terminal reply. The core-owned run boundary
 * distinguishes real users from synthetic goal prompts without inspecting
 * user-controlled prefixes; forward traversal avoids circular prev links. */
static char *last_reply(MotirisAgent *a) {
  const cJSON *start = motiris_turn_start(a);
  const char *reply = NULL;
  int goal_confirm = 0;
  for (const cJSON *m = start; m; m = m->next) {
    if (m != start && gateway_goal_prompt(m)) goal_confirm = 1;
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "role"));
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "content"));
    if (role && !strcmp(role, "assistant")) {
      cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
      int confirmation_call = goal_confirm && cJSON_IsArray(calls) &&
                              cJSON_GetArraySize(calls) > 0;
      int synthetic_done = goal_confirm && content &&
                           gateway_done_reply(content);
      /* Confirmation tools/DONE do not replace the real pre-goal answer. */
      if (!confirmation_call && !synthetic_done)
        reply = content && *content ? content : NULL;
    }
  }
  return reply ? strdup(reply) : NULL;
}

static void handle_connection(int fd, int tools_on) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return;
  }
  int read_status = 0;
  size_t body_len = 0;
  char *req = read_request(fd, &read_status, &body_len);
  if (!req) {
    const char *msg = read_status == 413 ? "request too large" :
                      read_status == 408 ? "request timeout" :
                      read_status == 500 ? "request processing failed" :
                      "malformed request";
    send_err(fd, read_status, msg);
    close(fd);
    return;
  }

  int bad_token = 0;
  cJSON *body = NULL;

  if (!check_token(req, &bad_token)) {
    send_err(fd, 401, "unauthorized");
    goto out;
  }
  if (!strncmp(req, "GET /health", 11)) {
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    send_json(fd, 200, ok);
    goto out;
  }
  if (strncmp(req, "POST /v1/chat", 13)) {
    send_err(fd, 404, "not found");
    goto out;
  }

  /* locate body after CRLFCRLF */
  char *bs = strstr(req, "\r\n\r\n");
  if (!bs) { send_err(fd, 400, "malformed request"); goto out; }
  int json_error = json_string_error(bs + 4, body_len);
  if (json_error) {
    send_err(fd, 400, json_error == 1 ? "embedded NUL is not allowed" :
                                      "invalid Unicode escape");
    goto out;
  }
  body = cJSON_ParseWithLength(bs + 4, body_len + 1);
  if (!body) { send_err(fd, 400, "invalid json body"); goto out; }

  const char *chat_id = NULL, *message = NULL;
  cJSON *jcid = cJSON_GetObjectItemCaseSensitive(body, "chat_id");
  cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(body, "message");
  cJSON *jrst = cJSON_GetObjectItemCaseSensitive(body, "reset");
  if (cJSON_IsString(jcid)) chat_id = jcid->valuestring;
  if (cJSON_IsString(jmsg)) message = jmsg->valuestring;
  if (!chat_id || !*chat_id || !message || !*message) {
    send_err(fd, 400, "chat_id and message required");
    goto out;
  }

  if (strlen(chat_id) > MAX_CHAT_ID_BYTES) {
    send_err(fd, 400, "chat_id exceeds 120-byte limit");
    goto out;
  }

  const char *session_error = NULL;
  if (cJSON_IsTrue(jrst)) { /* reset context and this exact ID's host snapshot */
#ifndef MOTIRIS_NO_PERSISTENCE
    if (motiris_session_auto_delete("gateway", chat_id, &session_error)) {
      send_err(fd, 500, session_error ? session_error : "cannot reset session");
      cJSON_Delete(body);
      free(req);
      close(fd);
      return;
    }
    char *path = session_log_path(chat_id);
    if (path && unlink(path) && errno != ENOENT) {
      free(path);
      send_err(fd, 500, "cannot reset session log");
      goto out;
    }
    free(path);
#endif
    Session *s = find_session(chat_id);
    if (s) {
      /* unlink from list */
      Session **pp = &sessions;
      while (*pp && *pp != s) pp = &(*pp)->next;
      *pp = s->next;
      session_dispose(s);
      n_sessions--;
    }
  }

  Session *s = get_session(chat_id, tools_on, &session_error);
  if (!s) {
    send_err(fd, 500, session_error ? session_error : "could not create session");
    goto out;
  }
  cJSON *jstream = cJSON_GetObjectItemCaseSensitive(body, "stream");
  int use_sse = !cJSON_IsFalse(jstream); /* SSE is the default wire format. */
  int saved_stream = motiris_stream_enabled(s->agent);
  int saved_thinking = motiris_thinking_visible(s->agent);
  SseWriter writer = { .fd = fd, .failed = 0 };
  if (use_sse) {
    motiris_set_stream(s->agent, 1);
    if (send_sse_start(&writer, fd)) {
      motiris_set_stream(s->agent, saved_stream);
      cJSON_Delete(body);
      free(req);
      close(fd);
      return;
    }
    motiris_set_stream_cb(s->agent, gateway_delta, &writer);
    motiris_set_reasoning_cb(s->agent, gateway_reasoning, &writer);
    /* Gateway always exposes reasoning as SSE, independently of the CLI's
     * show_thinking preference. Mark it visible so partial reasoning forbids
     * retry/fallback output from being mixed into the same client stream. */
    motiris_set_thinking_visible(s->agent, 1);
  } else {
    motiris_set_stream(s->agent, 0);
  }

  motiris_add_user(s->agent, message);
  int rc = motiris_run(s->agent);
  motiris_set_stream_cb(s->agent, NULL, NULL);
  motiris_set_reasoning_cb(s->agent, NULL, NULL);
  motiris_set_thinking_visible(s->agent, saved_thinking);
  motiris_set_stream(s->agent, saved_stream);
  const char *auto_checkpoint_error = NULL;
  int auto_saved = 0;
#ifndef MOTIRIS_NO_PERSISTENCE
  auto_saved = !motiris_session_auto_save(
      s->agent, s->session_id, "gateway", s->chat_id, &auto_checkpoint_error);
#endif
  if (rc) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", motiris_last_error(s->agent));
    if (use_sse) send_sse_event(&writer, "error", e);
    else send_json(fd, 500, e);
    goto out;
  }

  const char *checkpoint_error = NULL;
  if (s->log_path) {
    char encoded[MAX_CHAT_ID_ENCODED + 1];
    encode_chat_id(encoded, s->chat_id);
    if (motiris_session_save(s->log_path, s->agent, encoded,
                             &checkpoint_error)) {
      fprintf(stderr, "motiris gateway: checkpoint not saved: %s\n",
              checkpoint_error ? checkpoint_error : "snapshot failed");
    }
  }
  char *reply = last_reply(s->agent);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "reply", reply ? reply : "(no reply)");
  if (auto_checkpoint_error)
    cJSON_AddStringToObject(r, "checkpoint_error", auto_checkpoint_error);
  else if (checkpoint_error)
    cJSON_AddStringToObject(r, "checkpoint_error", checkpoint_error);
  else if (auto_saved)
    cJSON_AddStringToObject(r, "checkpoint", "saved");
  else
    cJSON_AddStringToObject(r, "checkpoint", "volatile-only");
  free(reply);
  if (use_sse) send_sse_event(&writer, "done", r);
  else send_json(fd, 200, r);

out:
  cJSON_Delete(body);
  free(req);
  close(fd);
}

int motiris_gateway_run(const char *listen_addr, const char *token,
                        int tools_on, int verbose) {
  gw_io_timeout_ms = GATEWAY_DEFAULT_TIMEOUT_S * 1000L;
  const char *raw_timeout = getenv("MOTIRIS_GATEWAY_IO_TIMEOUT_S");
  if (raw_timeout && *raw_timeout) {
    char *end = NULL;
    errno = 0;
    long timeout = strtol(raw_timeout, &end, 10);
    if (errno || !end || *end || timeout < 1 ||
        timeout > GATEWAY_MAX_TIMEOUT_S) {
      fprintf(stderr, "motiris gateway: MOTIRIS_GATEWAY_IO_TIMEOUT_S must be 1..%ld\n",
              GATEWAY_MAX_TIMEOUT_S);
      return 1;
    }
    gw_io_timeout_ms = timeout * 1000L;
  }
  gw_token = (token && *token) ? strdup(token) : NULL;
  gw_verbose = verbose;

  /* parse "host:port" | ":port" | "port" */
  const char *host = "0.0.0.0";
  char portstr[16] = "8899";
  if (listen_addr) {
    const char *colon = strrchr(listen_addr, ':');
    if (colon) {
      if (colon != listen_addr) {
        size_t hlen = (size_t)(colon - listen_addr);
        char hb[128];
        if (hlen < sizeof hb) { memcpy(hb, listen_addr, hlen); hb[hlen] = 0; host = strdup(hb); }
      }
      snprintf(portstr, sizeof portstr, "%s", colon + 1);
    } else {
      snprintf(portstr, sizeof portstr, "%s", listen_addr);
    }
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { fprintf(stderr, "motiris gateway: socket: %s\n", strerror(errno)); return 1; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)atoi(portstr));
  addr.sin_addr.s_addr = inet_addr(host);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "motiris gateway: bind %s:%s: %s\n", host, portstr, strerror(errno));
    return 1;
  }
  listen(fd, 16);
  signal(SIGPIPE, SIG_IGN);
  printf("motiris gateway listening on %s:%s\n", host, portstr);
  fflush(stdout);

  for (;;) {
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR) break;      /* clean shutdown via signal */
      continue;
    }
    handle_connection(cfd, tools_on);
  }
  close(fd);
  free(gw_token);
  return 0;
}