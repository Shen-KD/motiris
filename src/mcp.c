/* mcp.c - minimal MCP (Model Context Protocol) stdio client (issue #12).
 *
 * Speaks JSON-RPC 2.0 over newline-delimited stdio to an external MCP
 * server process (config: "mcp_servers": [{"name","cmd","args":[]}]).
 * On startup: initialize -> tools/list -> each remote tool registered
 * as a local motiris tool. Tool calls become tools/call requests.
 * Single-threaded: one in-flight request per server at a time.
 */
#include "motiris.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct McpConn {
  pid_t pid;
  FILE *wr;                 /* server stdin */
  int rd;                   /* server stdout */
  int next_id;
  char name[64];
} McpConn;

#define MAX_READ (1 << 20)

/* read one newline-delimited line from fd; returns malloc'd or NULL */
static char *read_line(int fd) {
  size_t cap = 1024, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    ssize_t n = read(fd, buf + len, 1);
    if (n <= 0) { free(buf); return NULL; }
    char c = buf[len];
    if (c == '\n') break;
    len++;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    if (len >= MAX_READ) break;
  }
  buf[len] = '\0';
  return buf;
}

static char *mcp_request(McpConn *c, const char *method, cJSON *params) {
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(req, "id", (double)(++c->next_id));
  cJSON_AddStringToObject(req, "method", method);
  if (params) cJSON_AddItemToObject(req, "params", params);
  char *line = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  fprintf(c->wr, "%s\n", line);
  fflush(c->wr);
  free(line);

  for (int tries = 0; tries < 64; tries++) {
    char *resp = read_line(c->rd);
    if (!resp) return NULL;
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) continue;
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(j, "id");
    if (!cJSON_IsNumber(jid) || (int)jid->valuedouble != c->next_id) {
      /* notification or other id - skip */
      cJSON_Delete(j);
      continue;
    }
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (cJSON_IsObject(err)) {
      char *msg = strdup(cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(err, "message")) ?: "mcp error");
      cJSON_Delete(j);
      return msg; /* caller must free; starts with not-"{" so detectable */
    }
    cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
    char *out = cJSON_IsObject(result) || cJSON_IsArray(result)
        ? cJSON_PrintUnformatted(result)
        : strdup(cJSON_GetStringValue(result) ?: "null");
    cJSON_Delete(j);
    return out;
  }
  return NULL;
}

static McpConn *mcp_connect(const char *name, const char *cmd,
                            const char *const *args) {
  int tos[2], froms[2];
  if (pipe(tos) || pipe(froms)) return NULL;

  pid_t pid = fork();
  if (pid < 0) return NULL;
  if (pid == 0) {
    dup2(tos[0], 0);
    dup2(froms[1], 1);
    close(tos[0]); close(tos[1]);
    close(froms[0]); close(froms[1]);
    int n = 0;
    while (args && args[n]) n++;
    char **av = malloc((size_t)(n + 2) * sizeof(char *));
    av[0] = (char *)cmd;
    for (int i = 0; i < n; i++) av[i + 1] = (char *)args[i];
    av[n + 1] = NULL;
    execvp(cmd, av);
    _exit(127);
  }

  close(tos[0]);
  close(froms[1]);
  McpConn *c = calloc(1, sizeof *c);
  c->pid = pid;
  c->wr = fdopen(tos[1], "w");
  c->rd = froms[0];
  c->next_id = 0;
  snprintf(c->name, sizeof c->name, "%s", name);
  return c;
}

static void mcp_close(McpConn *c) {
  if (!c) return;
  fclose(c->wr);
  close(c->rd);
  kill(c->pid, SIGTERM);
  int st;
  waitpid(c->pid, &st, 0);
  free(c);
}

/* ---------------- tool bridge ---------------- */
typedef struct { McpConn *conn; char tname[128]; } McpToolUdata;

static char *mcp_tool_call(const char *args_json, void *ud) {
  McpToolUdata *u = ud;
  cJSON *params = cJSON_CreateObject();
  cJSON_AddStringToObject(params, "name", u->tname);
  cJSON *args = cJSON_Parse(args_json);
  cJSON_AddItemToObject(params, "arguments", args ? args : cJSON_Parse("{}"));

  char *raw = mcp_request(u->conn, "tools/call", params);
  if (!raw) return strdup("{\"error\":\"mcp server unreachable\"}");

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
    fprintf(stderr, "motiris mcp: %s initialize failed\n", name);
    mcp_close(conn);
    return -1;
  }
  /* initialized notification (no id) */
  cJSON *note = cJSON_CreateObject();
  cJSON_AddStringToObject(note, "jsonrpc", "2.0");
  cJSON_AddStringToObject(note, "method", "notifications/initialized");
  char *nl = cJSON_PrintUnformatted(note);
  fprintf(conn->wr, "%s\n", nl);
  fflush(conn->wr);
  free(nl);
  cJSON_Delete(note);

  char *tl = mcp_request(conn, "tools/list", NULL);
  if (!tl) {
    fprintf(stderr, "motiris mcp: %s tools/list failed\n", name);
    mcp_close(conn);
    return -1;
  }
  cJSON *tj = cJSON_Parse(tl);
  free(tl);
  if (!tj) { mcp_close(conn); return -1; }

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
    /* register_tool shallow-copies the struct, so keep it on the heap */
    MotirisTool *mt = calloc(1, sizeof *mt);
    mt->name = strdup(label);
    mt->description = desc ? strdup(desc) : strdup("MCP tool");
    /* schema: reuse server-provided inputSchema if present, else open */
    cJSON *sch = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
    mt->parameters = sch ? strdup(cJSON_PrintUnformatted(sch))
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