/* tools.c - built-in tools for motiris.
 *
 * Tool contract: a tool is (name, description, parameters-json, handler).
 * The handler receives the arguments as JSON text and must return a
 * malloc'd JSON string (any shape); the agent wraps it as the model's
 * "tool" role message.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---------------- shell: run a command, capture output ---------------- */
static void capture_out(int fd, char **acc, size_t *len) {
  char tmp[4096];
  ssize_t n;
  while ((n = read(fd, tmp, sizeof tmp)) > 0) {
    *acc = realloc(*acc, *len + (size_t)n + 1);
    memcpy(*acc + *len, tmp, (size_t)n);
    *len += (size_t)n;
    (*acc)[*len] = '\0';
  }
}

static char *shell_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return cJSON_PrintUnformatted(
      cJSON_CreateString("error: cannot parse arguments json"));
  cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
  if (!cJSON_IsString(cmd) || !cmd->valuestring) {
    cJSON_Delete(args);
    return cJSON_PrintUnformatted(cJSON_CreateString(
        "error: missing string field \"command\""));
  }
  const char *command = cmd->valuestring;

  char *ban = motiris_shell_policy_check(command);
  if (ban) {
    cJSON_Delete(args);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", ban);
    free(ban);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
  }

  int outpipe[2], errpipe[2];
  if (pipe(outpipe) || pipe(errpipe)) {
    cJSON_Delete(args);
    return cJSON_PrintUnformatted(cJSON_CreateString("error: pipe failed"));
  }
  pid_t pid = fork();
  if (pid < 0) { cJSON_Delete(args); return cJSON_PrintUnformatted(
      cJSON_CreateString("error: fork failed")); }

  int code = 0;
  char *out = NULL, *err = NULL;
  size_t olen = 0, elen = 0;

  if (pid == 0) {
    dup2(outpipe[1], 1);
    dup2(errpipe[1], 2);
    close(outpipe[0]); close(outpipe[1]);
    close(errpipe[0]); close(errpipe[1]);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }

  close(outpipe[1]);
  close(errpipe[1]);
  capture_out(outpipe[0], &out, &olen);
  capture_out(errpipe[0], &err, &elen);
  close(outpipe[0]);
  close(errpipe[0]);
  waitpid(pid, &code, 0);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "stdout", out ? out : "");
  cJSON_AddStringToObject(r, "stderr", err ? err : "");
  cJSON_AddNumberToObject(r, "exit_code",
      WIFEXITED(code) ? WEXITSTATUS(code) : -1);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  free(out);
  free(err);
  return s;
}

/* ---------------- time: current UTC clock ---------------- */
static char *time_call(const char *args_json, void *ud) {
  (void)args_json; (void)ud;
  char iso[64];
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "utc_iso8601", iso);
  cJSON_AddNumberToObject(r, "unix_seconds", (double)now);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- registration ---------------- */
static const MotirisTool core_tools[] = {
  { "shell",
    "Run a shell command via /bin/sh and capture stdout, stderr and "
    "exit code. The command runs with the agent's uid - it can modify "
    "the system. Use for file inspection, git, build tools, networking.",
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
    "\"description\":\"shell command line\"}},\"required\":[\"command\"]}",
    shell_call, NULL },
  { "time",
    "Get the current UTC time as ISO-8601 and unix seconds.",
    "{\"type\":\"object\",\"properties\":{}}",
    time_call, NULL },
};

int motiris_register_core_tools_count(void) {
  return (int)(sizeof core_tools / sizeof core_tools[0]);
}
const MotirisTool *motiris_core_tools(void) { return core_tools; }