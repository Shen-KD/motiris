/* gateway.c - embedded HTTP gateway for motiris.
 *
 * Turns motiris into a resident service (like a message-platform
 * gateway, minus the platform): each chat_id gets its own agent
 * instance (its own context), so many conversations can run on one
 * process. Request handling is sequential (single-threaded, POSIX
 * poll-free accept loop) - fine for modest traffic, documented limit.
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SESSIONS 64
#define MAX_CHAT_ID_BYTES 120
#define MAX_CHAT_ID_ENCODED (MAX_CHAT_ID_BYTES * 2)
#define MAX_HEADER (16 * 1024)
#define MAX_BODY (1 << 20)

/* ---------------- session table ---------------- */
typedef struct Session {
  char *chat_id;
  MotirisAgent *agent;
  char *log_path;
  struct Session *next;
} Session;

static Session *sessions;
static int n_sessions;
static char *gw_token;
static int gw_verbose;

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
  motiris_free(p->agent);
  free(p->chat_id);
  free(p->log_path);
  free(p);
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

static Session *get_session(const char *chat_id, int tools_on,
                            const char **error) {
  Session *s = find_session(chat_id);
  if (s) return s;
  *error = "could not create session";

  s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->chat_id = strdup(chat_id);
  s->agent = motiris_new();
  if (!s->chat_id || !s->agent) goto fail;
  motiris_apply_config(s->agent);   /* model/base_url/transport from config */
  if (tools_on) motiris_register_core_tools(s->agent);

  /* Per-chat log: gateway/<lowercase-hex(chat_id)>.log. */
  if (getenv("HOME")) {
    s->log_path = session_log_path(chat_id);
    if (!s->log_path) goto fail;
    char *dir = strdup(s->log_path);
    if (!dir) goto fail;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    mkdir_p(dir);
    free(dir);
    if (load_session_log(s, error)) goto fail;
  }
  if (n_sessions >= MAX_SESSIONS) evict_oldest();
  s->next = sessions;
  sessions = s;
  n_sessions++;
  return s;

fail:
  motiris_free(s->agent);
  free(s->chat_id);
  free(s->log_path);
  free(s);
  return NULL;
}

/* ---------------- HTTP plumbing ---------------- */
static void send_json(int fd, int code, cJSON *body) {
  char *s = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  char hdr[256];
  const char *reason = code == 200 ? "OK" :
                       code == 401 ? "Unauthorized" :
                       code == 404 ? "Not Found" :
                       code == 413 ? "Payload Too Large" :
                       code == 500 ? "Internal Server Error" : "Bad Request";
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
           "Content-Length: %zu\r\nConnection: close\r\n\r\n",
           code, reason, strlen(s));
  ssize_t w1 = write(fd, hdr, strlen(hdr));
  ssize_t w2 = write(fd, s, strlen(s));
  (void)w1; (void)w2;
  free(s);
}

static void send_err(int fd, int code, const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  send_json(fd, code, e);
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

/* Read one bounded request. The allocation is at most
 * MAX_HEADER + MAX_BODY + 1 bytes, including the trailing NUL. */
static char *read_request(int fd, int *status) {
  size_t cap = 8192, len = 0, header_len = 0, body_len = 0, searched = 0;
  char *buf = malloc(cap + 1);
  *status = 500;
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
    ssize_t n = read(fd, buf + len, cap - len);
    if (n < 0) {
      if (errno == EINTR) continue;
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

/* return the last assistant content (final reply), or NULL */
static char *last_reply(MotirisAgent *a) {
  cJSON *m = motiris_messages(a)->child;
  if (!m) return NULL;
  while (m->next) m = m->next;
  for (; m; m = m->prev) {
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "role"));
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "content"));
    if (role && !strcmp(role, "assistant") && content && *content)
      return strdup(content);
  }
  return NULL;
}

static void handle_connection(int fd, int tools_on) {
  int read_status = 0;
  char *req = read_request(fd, &read_status);
  if (!req) {
    const char *msg = read_status == 413 ? "request too large" :
                      read_status == 500 ? "out of memory" : "malformed request";
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
  body = cJSON_Parse(bs + 4);
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

  if (cJSON_IsTrue(jrst)) { /* reset: drop only this exact chat_id's log */
    char *path = session_log_path(chat_id);
    if (path && unlink(path) && errno != ENOENT) {
      free(path);
      send_err(fd, 500, "cannot reset session log");
      goto out;
    }
    free(path);
    Session *s = find_session(chat_id);
    if (s) {
      motiris_free(s->agent);
      /* unlink from list */
      Session **pp = &sessions;
      while (*pp && *pp != s) pp = &(*pp)->next;
      *pp = s->next;
      free(s->chat_id); free(s->log_path); free(s);
      n_sessions--;
    }
  }

  const char *session_error = NULL;
  Session *s = get_session(chat_id, tools_on, &session_error);
  if (!s) {
    send_err(fd, 500, session_error ? session_error : "could not create session");
    goto out;
  }
  motiris_add_user(s->agent, message);

  int rc = motiris_run(s->agent);
  if (rc) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", motiris_last_error(s->agent));
    send_json(fd, 500, e);
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
  if (!s->log_path)
    cJSON_AddStringToObject(r, "checkpoint", "volatile-only");
  else if (checkpoint_error)
    cJSON_AddStringToObject(r, "checkpoint_error", checkpoint_error);
  else
    cJSON_AddStringToObject(r, "checkpoint", "saved");
  free(reply);
  send_json(fd, 200, r);

out:
  cJSON_Delete(body);
  free(req);
  close(fd);
}

int motiris_gateway_run(const char *listen_addr, const char *token,
                        int tools_on, int verbose) {
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