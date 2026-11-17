/* mcp.c - bounded MCP (Model Context Protocol) stdio client (issue #12).
 *
 * Speaks JSON-RPC 2.0 over newline-delimited stdio to an external MCP
 * server process (config: "mcp_servers": [{"name","cmd","args":[]}]).
 * On startup: initialize -> tools/list -> each remote tool registered
 * as a local motiris tool. Tool calls become tools/call requests.
 * Single-threaded: one in-flight request per server at a time. Every
 * request has a monotonic deadline; process cleanup has a bounded TERM grace
 * before KILL/reap. Failed connections retain only bounded diagnostics for
 * subsequent tool calls.
 */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MCP_MAX_READ (1 << 20)
#define MCP_INPUT_CAP 4096
#define MCP_DEFAULT_TIMEOUT_S 10
#define MCP_MAX_TIMEOUT_S 120
#define MCP_CLOSE_GRACE_MS 1000L
#define MCP_WAIT_SLICE_MS 50L

typedef struct McpConn {
  pid_t pid;
  int wr;                   /* server stdin */
  int rd;                   /* server stdout */
  int next_id;
  int timeout_s;
  int failed;
  char name[64];
  char error[192];
  unsigned char input[MCP_INPUT_CAP];
  size_t input_pos;
  size_t input_len;
} McpConn;

static void mcp_set_error(McpConn *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->error, sizeof c->error, fmt, ap);
  va_end(ap);
}

static long mcp_ms_remaining(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  long remaining = (deadline->tv_sec - now.tv_sec) * 1000L
                 + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
  return remaining < 0 ? 0 : remaining;
}

static int mcp_deadline(McpConn *c, struct timespec *deadline) {
  if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
    mcp_set_error(c, "mcp monotonic clock unavailable");
    return -1;
  }
  deadline->tv_sec += c->timeout_s;
  return 0;
}

/* Wait without ever extending the caller's absolute deadline. */
static int mcp_poll(McpConn *c, int fd, short events,
                    const struct timespec *deadline) {
  for (;;) {
    long remaining = mcp_ms_remaining(deadline);
    if (remaining < 0) {
      mcp_set_error(c, "mcp monotonic clock failed");
      return -1;
    }
    if (remaining == 0) {
      mcp_set_error(c, "mcp request %s timed out after %d s",
                    events == POLLOUT ? "write" : "read", c->timeout_s);
      return -1;
    }
    struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
    int ready = poll(&pfd, 1, remaining > 1000L ? 1000 : (int)remaining);
    if (ready < 0) {
      if (errno == EINTR) continue;
      mcp_set_error(c, "mcp poll failed: %.120s", strerror(errno));
      return -1;
    }
    if (ready == 0) continue;
    if (pfd.revents & POLLNVAL) {
      mcp_set_error(c, "mcp invalid pipe descriptor");
      return -1;
    }
    return pfd.revents;
  }
}

static void mcp_terminate(McpConn *c) {
  if (c->pid <= 0) return;

  /* Signal before reaping: an exited leader may leave descendants behind. */
  kill(-c->pid, SIGTERM);
  kill(c->pid, SIGTERM);

  int status;
  pid_t waited;
  do {
    waited = waitpid(c->pid, &status, WNOHANG);
  } while (waited < 0 && errno == EINTR);
  if (waited == c->pid || (waited < 0 && errno == ECHILD)) {
    kill(-c->pid, SIGKILL); /* The leader is gone; descendants get no reprieve. */
    c->pid = -1;
    return;
  }

  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) {
    deadline.tv_sec += MCP_CLOSE_GRACE_MS / 1000L;
    deadline.tv_nsec += (MCP_CLOSE_GRACE_MS % 1000L) * 1000000L;
    for (;;) {
      do {
        waited = waitpid(c->pid, &status, WNOHANG);
      } while (waited < 0 && errno == EINTR);
      if (waited == c->pid || (waited < 0 && errno == ECHILD)) {
        kill(-c->pid, SIGKILL);
        c->pid = -1;
        return;
      }
      long remaining = mcp_ms_remaining(&deadline);
      if (remaining <= 0) break;
      long slice = remaining < MCP_WAIT_SLICE_MS ? remaining : MCP_WAIT_SLICE_MS;
      (void)poll(NULL, 0, (int)slice); /* EINTR rechecks the same deadline. */
    }
  }

  kill(-c->pid, SIGKILL);
  kill(c->pid, SIGKILL);
  while (waitpid(c->pid, &status, 0) < 0) {
    if (errno != EINTR) break;
  }
  c->pid = -1;
}

/* Keep the bounded connection/error object alive for registered tool udata. */
static void mcp_abort(McpConn *c) {
  c->failed = 1;
  if (c->wr >= 0) close(c->wr);
  if (c->rd >= 0) close(c->rd);
  c->wr = -1;
  c->rd = -1;
  mcp_terminate(c);
}

static void mcp_close(McpConn *c) {
  if (!c) return;
  mcp_abort(c);
  free(c);
}

/* Write one bounded line without allowing a dead peer's SIGPIPE to kill us. */
static int mcp_write_line(McpConn *c, const char *line,
                          const struct timespec *deadline) {
  size_t len = strlen(line);
  if (len > MCP_MAX_READ) {
    mcp_set_error(c, "mcp request exceeds %d bytes", MCP_MAX_READ);
    return -1;
  }

  struct sigaction ignore = {0}, old = {0};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (sigaction(SIGPIPE, &ignore, &old) != 0) {
    mcp_set_error(c, "mcp cannot configure pipe signal handling");
    return -1;
  }

  size_t offset = 0;
  int result = -1;
  while (offset < len + 1) {
    int events = mcp_poll(c, c->wr, POLLOUT, deadline);
    if (events < 0) break;
    if (!(events & POLLOUT)) {
      if (events & (POLLERR | POLLHUP)) {
        mcp_set_error(c, "mcp server closed stdin");
        break;
      }
      continue;
    }
    const char *data = offset < len ? line + offset : "\n";
    size_t amount = offset < len ? len - offset : 1;
    ssize_t written = write(c->wr, data, amount);
    if (written > 0) {
      offset += (size_t)written;
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN
                        || errno == EWOULDBLOCK))
      continue;
    mcp_set_error(c, "mcp server closed stdin");
    break;
  }
  if (offset == len + 1) result = 0;
  if (sigaction(SIGPIPE, &old, NULL) != 0 && result == 0) {
    mcp_set_error(c, "mcp cannot restore pipe signal handling");
    result = -1;
  }
  return result;
}

/* Return one byte, retaining read-ahead for notifications and later replies. */
static int mcp_read_byte(McpConn *c, const struct timespec *deadline) {
  if (c->input_pos < c->input_len)
    return c->input[c->input_pos++];

  for (;;) {
    int events = mcp_poll(c, c->rd, POLLIN, deadline);
    if (events < 0) return -1;
    if (!(events & (POLLIN | POLLHUP | POLLERR))) continue;
    ssize_t n = read(c->rd, c->input, sizeof c->input);
    if (n > 0) {
      c->input_pos = 1;
      c->input_len = (size_t)n;
      return c->input[0];
    }
    if (n == 0) {
      mcp_set_error(c, "mcp server closed stdout");
      return -1;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    mcp_set_error(c, "mcp read failed: %.120s", strerror(errno));
    return -1;
  }
}

/* Read one newline-delimited line from fd; returns malloc'd or NULL. */
static char *mcp_read_line(McpConn *c, const struct timespec *deadline) {
  size_t cap = 1024, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    mcp_set_error(c, "mcp response allocation failed");
    return NULL;
  }
  for (;;) {
    int ch = mcp_read_byte(c, deadline);
    if (ch < 0) {
      free(buf);
      return NULL;
    }
    if (ch == '\n') break;
    if (len >= MCP_MAX_READ) {
      mcp_set_error(c, "mcp response exceeds %d bytes", MCP_MAX_READ);
      free(buf);
      return NULL;
    }
    if (len + 2 > cap) {
      size_t next = cap * 2;
      if (next > MCP_MAX_READ + 1) next = MCP_MAX_READ + 1;
      char *grown = realloc(buf, next);
      if (!grown) {
        mcp_set_error(c, "mcp response allocation failed");
        free(buf);
        return NULL;
      }
      buf = grown;
      cap = next;
    }
    buf[len++] = (char)ch;
  }
  buf[len] = '\0';
  return buf;
}

/* Takes ownership of params, including immediate failure and retry paths. */
static char *mcp_request(McpConn *c, const char *method, cJSON *params) {
  if (c->failed) {
    cJSON_Delete(params);
    return NULL;
  }
  c->error[0] = '\0';

  struct timespec deadline;
  if (mcp_deadline(c, &deadline) != 0) {
    cJSON_Delete(params);
    goto fatal;
  }

  cJSON *req = cJSON_CreateObject();
  if (!req) {
    cJSON_Delete(params);
    mcp_set_error(c, "mcp request allocation failed");
    goto fatal;
  }
  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(req, "id", (double)(++c->next_id));
  cJSON_AddStringToObject(req, "method", method);
  if (params) cJSON_AddItemToObject(req, "params", params);
  char *line = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!line) {
    mcp_set_error(c, "mcp request allocation failed");
    goto fatal;
  }
  int write_result = mcp_write_line(c, line, &deadline);
  free(line);
  if (write_result != 0) goto fatal;

  for (int tries = 0; tries < 64; tries++) {
    char *resp = mcp_read_line(c, &deadline);
    if (!resp) goto fatal;
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) continue;
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(j, "id");
    if (!cJSON_IsNumber(jid) || (int)jid->valuedouble != c->next_id) {
      /* Notification or another id: skip within the same request deadline. */
      cJSON_Delete(j);
      continue;
    }
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (cJSON_IsObject(err)) {
      const char *msg = cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(err, "message"));
      mcp_set_error(c, "mcp server error: %.140s", msg ? msg : "unknown");
      cJSON_Delete(j);
      return NULL; /* Protocol error: the transport itself remains usable. */
    }
    cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
    char *out = cJSON_IsObject(result) || cJSON_IsArray(result)
        ? cJSON_PrintUnformatted(result)
        : strdup(cJSON_GetStringValue(result) ?: "null");
    cJSON_Delete(j);
    if (!out) {
      mcp_set_error(c, "mcp response allocation failed");
      goto fatal;
    }
    return out;
  }
  mcp_set_error(c, "mcp response notification limit exceeded");

fatal:
  mcp_abort(c);
  return NULL;
}

/* ---------------- tool bridge ---------------- */
typedef struct { McpConn *conn; char tname[128]; } McpToolUdata;

static char *mcp_json_error(const char *message) {
  cJSON *out = cJSON_CreateObject();
  if (!out) return strdup("{\"error\":\"mcp request failed\"}");
  cJSON_AddStringToObject(out, "error", message && *message
                          ? message : "mcp server unreachable");
  char *printed = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  return printed;
}

static char *mcp_tool_call(const char *args_json, void *ud) {
  McpToolUdata *u = ud;
  cJSON *params = cJSON_CreateObject();
  if (!params) return mcp_json_error("mcp request allocation failed");
  cJSON_AddStringToObject(params, "name", u->tname);
  cJSON *args = cJSON_Parse(args_json);
  cJSON_AddItemToObject(params, "arguments", args ? args : cJSON_Parse("{}"));

  char *raw = mcp_request(u->conn, "tools/call", params);
  if (!raw) return mcp_json_error(u->conn->error);

  cJSON *r = cJSON_Parse(raw);
  free(raw);
  if (!r) return strdup("{\"error\":\"mcp invalid response\"}");

  /* result.content: [{type:"text",text:"..."}] -> join text fields */
  cJSON *content = cJSON_GetObjectItemCaseSensitive(r, "content");
  cJSON *out = cJSON_CreateObject();
  if (cJSON_IsArray(content)) {
    size_t cap = 2048, len = 0;
    char *text = malloc(cap);
    text[0] = '\0';
    cJSON *item;
    cJSON_ArrayForEach(item, content) {
      const char *t = cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(item, "text"));
      if (!t) continue;
      size_t tl = strlen(t);
      if (len + tl + 2 >= cap) { cap += tl + 4096; text = realloc(text, cap); }
      memcpy(text + len, t, tl);
      len += tl;
      text[len++] = '\n';
      text[len] = '\0';
    }
    cJSON_AddStringToObject(out, "content", text);
    free(text);
  } else {
    char *s = cJSON_PrintUnformatted(r);
    cJSON_AddStringToObject(out, "content", s);
    free(s);
  }
  cJSON_Delete(r);
  char *s = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  return s;
}

/* ---------------- connect + register ---------------- */
static int mcp_parse_timeout(void) {
  const char *raw = getenv("MOTIRIS_MCP_TIMEOUT_S");
  if (!raw || !*raw) return MCP_DEFAULT_TIMEOUT_S;
  char *end = NULL;
  errno = 0;
  long value = strtol(raw, &end, 10);
  if (errno || !end || *end || value < 1 || value > MCP_MAX_TIMEOUT_S) {
    fprintf(stderr, "motiris mcp: MOTIRIS_MCP_TIMEOUT_S must be 1..120\n");
    return -1;
  }
  return (int)value;
}

static int mcp_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static McpConn *mcp_connect(const char *name, const char *cmd,
                            const char *const *args) {
  int timeout_s = mcp_parse_timeout();
  if (timeout_s < 0) return NULL;

  McpConn *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->pid = -1;
  c->wr = -1;
  c->rd = -1;
  c->timeout_s = timeout_s;
  snprintf(c->name, sizeof c->name, "%s", name);

  int tos[2], froms[2];
  if (pipe(tos) != 0) {
    free(c);
    return NULL;
  }
  if (pipe(froms) != 0) {
    close(tos[0]);
    close(tos[1]);
    free(c);
    return NULL;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(tos[0]); close(tos[1]);
    close(froms[0]); close(froms[1]);
    free(c);
    return NULL;
  }
  if (pid == 0) {
    if (setsid() < 0) _exit(126);
    if (dup2(tos[0], STDIN_FILENO) < 0
        || dup2(froms[1], STDOUT_FILENO) < 0)
      _exit(126);
    close(tos[0]); close(tos[1]);
    close(froms[0]); close(froms[1]);
    int n = 0;
    while (args && args[n]) n++;
    char **av = malloc((size_t)(n + 2) * sizeof(char *));
    if (!av) _exit(127);
    av[0] = (char *)cmd;
    for (int i = 0; i < n; i++) av[i + 1] = (char *)args[i];
    av[n + 1] = NULL;
    execvp(cmd, av);
    _exit(127);
  }

  close(tos[0]);
  close(froms[1]);
  c->pid = pid;
  c->wr = tos[1];
  c->rd = froms[0];
  if (mcp_nonblock(c->wr) != 0 || mcp_nonblock(c->rd) != 0) {
    mcp_close(c);
    return NULL;
  }
  return c;
}

int motiris_register_mcp_server(MotirisAgent *a, const char *name,
                                const char *cmd, const char *const *args) {
  McpConn *conn = mcp_connect(name, cmd, args);
  if (!conn) {
    fprintf(stderr, "motiris mcp: cannot start %s (%s)\n", name, cmd);
    return -1;
  }

  char *init = mcp_request(conn, "initialize", NULL);
  free(init);
  if (!init) {
    fprintf(stderr, "motiris mcp: %s initialize failed: %s\n",
            name, conn->error);
    mcp_close(conn);
    return -1;
  }
  /* Initialized notification shares the same bounded nonblocking write path. */
  cJSON *note = cJSON_CreateObject();
  cJSON_AddStringToObject(note, "jsonrpc", "2.0");
  cJSON_AddStringToObject(note, "method", "notifications/initialized");
  char *nl = cJSON_PrintUnformatted(note);
  cJSON_Delete(note);
  struct timespec deadline;
  if (!nl || mcp_deadline(conn, &deadline) != 0
      || mcp_write_line(conn, nl ? nl : "", &deadline) != 0) {
    free(nl);
    fprintf(stderr, "motiris mcp: %s initialized notification failed: %s\n",
            name, conn->error);
    mcp_close(conn);
    return -1;
  }
  free(nl);

  char *tl = mcp_request(conn, "tools/list", NULL);
  if (!tl) {
    fprintf(stderr, "motiris mcp: %s tools/list failed: %s\n",
            name, conn->error);
    mcp_close(conn);
    return -1;
  }
  cJSON *tj = cJSON_Parse(tl);
  free(tl);
  if (!tj) {
    mcp_set_error(conn, "mcp invalid tools/list response");
    mcp_close(conn);
    return -1;
  }

  int n = 0;
  cJSON *tools = cJSON_GetObjectItemCaseSensitive(tj, "tools");
  cJSON *t;
  cJSON_ArrayForEach(t, tools) {
    const char *tname = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(t, "name"));
    const char *desc = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(t, "description"));
    if (!tname || !*tname) continue;
    McpToolUdata *u = calloc(1, sizeof *u);
    u->conn = conn;
    snprintf(u->tname, sizeof u->tname, "%s", tname);
    char label[160];
    snprintf(label, sizeof label, "%s:%s", name, tname);
    /* register_tool shallow-copies the struct, so keep it on the heap. */
    MotirisTool *mt = calloc(1, sizeof *mt);
    mt->name = strdup(label);
    mt->description = desc ? strdup(desc) : strdup("MCP tool");
    /* schema: reuse server-provided inputSchema if present, else open */
    cJSON *sch = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
    mt->parameters = sch ? cJSON_PrintUnformatted(sch)
                         : strdup("{\"type\":\"object\"}");
    mt->call = mcp_tool_call;
    mt->ud = u;
    motiris_register_tool(a, mt);
    n++;
  }
  cJSON_Delete(tj);
  if (n == 0) {
    fprintf(stderr, "motiris mcp: %s exposes no tools\n", name);
    mcp_close(conn);
    return -1;
  }
  return 0;
}
