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
 * Sessions are append-only logged under ~/.local/share/motiris/
 * gateway/<chat_id>.log (U/A/T rows, same format as --save) and user
 * history is replayed when a session file exists.
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

/* sanitize chat_id into a safe file name (keep [A-Za-z0-9._-]) */
static void safe_name(char *out, size_t n, const char *id) {
  size_t j = 0;
  for (const char *c = id; *c && j + 1 < n; c++) {
    char ch = *c;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')
      out[j++] = ch;
    else
      out[j++] = '_';
  }
  out[j] = '\0';
}

static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static Session *get_session(const char *chat_id, int tools_on) {
  Session *s = find_session(chat_id);
  if (s) return s;
  if (n_sessions >= MAX_SESSIONS) evict_oldest();

  s = calloc(1, sizeof *s);
  s->chat_id = strdup(chat_id);
  s->agent = motiris_new();
  motiris_apply_config(s->agent);   /* model/base_url/transport from config */
  if (tools_on) motiris_register_core_tools(s->agent);

  /* per-chat session log: $HOME/.local/share/motiris/gateway/<chat>.log */
  const char *h = getenv("HOME");
  if (h) {
    char ch[128];
    safe_name(ch, sizeof ch, chat_id);
    size_t n = strlen(h) + 64;
    char *dir = malloc(n);
    snprintf(dir, n, "%s/.local/share/motiris/gateway", h);
    mkdir_p(dir);
    s->log_path = malloc(n);
    snprintf(s->log_path, n, "%s/%s.log", dir, ch);
    free(dir);
    /* replay user history so context survives restarts */
    FILE *f = fopen(s->log_path, "r");
    if (f) {
      char line[65536];
      while (fgets(line, sizeof line, f))
        if (line[0] == 'U') motiris_add_user(s->agent, line + 1);
      fclose(f);
    }
  }
  s->next = sessions;
  sessions = s;
  n_sessions++;
  return s;
}

/* append messages added since the given index to the session log */
static void log_new_messages(Session *s, int from_index) {
  if (!s->log_path) return;
  FILE *f = fopen(s->log_path, "a");
  if (!f) return;
  cJSON *msgs = motiris_messages(s->agent);
  int i = 0;
  for (cJSON *m = msgs->child; m; m = m->next, i++) {
    if (i < from_index) continue;
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "role"));
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "content"));
    if (!role || !role[0]) continue;
    char tag;
    switch (role[0]) {
      case 'u': tag = 'U'; break;
      case 'a': tag = 'A'; break;
      case 't': tag = 'T'; break;
      default: continue;
    }
    fprintf(f, "%c%s\n", tag, content ? content : "");
  }
  fclose(f);
}

/* ---------------- HTTP plumbing ---------------- */
static void send_json(int fd, int code, cJSON *body) {
  char *s = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  char hdr[256];
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
           "Content-Length: %zu\r\nConnection: close\r\n\r\n",
           code, code == 200 ? "OK" : (code == 404 ? "Not Found"
                            : "Bad Request"), strlen(s));
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

/* read full request (headers + body); returns malloc'd request, or NULL */
static char *read_request(int fd) {
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    ssize_t n = read(fd, buf + len, cap - len - 1);
    if (n <= 0) break;
    len += (size_t)n;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    /* parse Content-Length to know when the body is complete */
    if (strstr(buf, "\r\n\r\n")) {
      char *cl = strstr(buf, "Content-Length:");
      long body = 0;
      if (cl) body = atol(cl + 15);
      char *head_end = strstr(buf, "\r\n\r\n");
      long have = (long)(len - (size_t)(head_end - buf) - 4);
      if (have >= body) break;
    }
  }
  buf[len] = '\0';
  return buf;
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
  char *req = read_request(fd);
  if (!req) { close(fd); return; }

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

  if (cJSON_IsTrue(jrst)) { /* reset: drop remembered context */
    Session *s = find_session(chat_id);
    if (s) {
      motiris_free(s->agent);
      if (s->log_path) unlink(s->log_path);
      /* unlink from list */
      Session **pp = &sessions;
      while (*pp && *pp != s) pp = &(*pp)->next;
      *pp = s->next;
      free(s->chat_id); free(s->log_path); free(s);
      n_sessions--;
    }
  }

  Session *s = get_session(chat_id, tools_on);
  int before = (int)cJSON_GetArraySize(motiris_messages(s->agent));
  motiris_add_user(s->agent, message);

  int rc = motiris_run(s->agent);
  log_new_messages(s, before);
  if (rc) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", motiris_last_error(s->agent));
    send_json(fd, 500, e);
    goto out;
  }

  char *reply = last_reply(s->agent);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "reply", reply ? reply : "(no reply)");
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