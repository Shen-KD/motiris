/* subagent.c - subagent tool (issue #9).
 *
 * Spawns a fresh `motiris -p <task> --no-tools` child process and
 * returns its output. The child gets zero tools, so a delegated task
 * can never escape through the parent's shell/filesystem surface; the
 * parent keeps its own context untouched. Subagent env is inherited;
 * override binary with MOTIRIS_BIN.
 */
#include "motiris.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SUB_OUT_CAP (32 * 1024)

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

static char *own_bin(void) {
  const char *env = getenv("MOTIRIS_BIN");
  if (env && *env) return strdup(env);
  char link[4096];
  ssize_t n = readlink("/proc/self/exe", link, sizeof link - 1);
  if (n > 0) { link[n] = '\0'; return strdup(link); }
  return strdup("motiris");
}

static char *subagent_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *task = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "task"));
  if (!task || !*task) { cJSON_Delete(args); return err_json("task required"); }

  char *bin = own_bin();

  int outpipe[2];
  if (pipe(outpipe)) {
    free(bin); cJSON_Delete(args);
    return err_json("pipe failed");
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(bin); cJSON_Delete(args);
    return err_json("fork failed");
  }
  if (pid == 0) {
    /* child: motiris -p <task> --no-tools [--max-steps 3] */
    dup2(outpipe[1], 1);
    dup2(outpipe[1], 2);
    close(outpipe[0]);
    close(outpipe[1]);
    execl(bin, bin, "-p", task, "--no-tools", "--max-steps", "3",
          (char *)NULL);
    _exit(127);
  }

  close(outpipe[1]);
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  ssize_t n;
  while ((n = read(outpipe[0], buf + len, cap - len - 1)) > 0) {
    len += (size_t)n;
    if (len + 1 >= cap) {
      if (cap >= SUB_OUT_CAP) break;
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }
  close(outpipe[0]);
  buf[len] = '\0';

  int wstatus = 0;
  while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}
  int rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

  cJSON_Delete(args);
  free(bin);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "output", buf);
  cJSON_AddNumberToObject(r, "exit_code", (double)rc);
  free(buf);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static const MotirisTool subagent_tools[] = {
  { "subagent",
    "Delegate a self-contained task to a fresh motiris process (no tools) "
    "and return its output. Use for independent subtasks; keep the parent "
    "context small.",
    "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"}},"
    "\"required\":[\"task\"]}",
    subagent_call, NULL },
};

void motiris_register_subagent_tools(MotirisAgent *a) {
  for (size_t i = 0; i < sizeof subagent_tools / sizeof subagent_tools[0]; i++)
    motiris_register_tool(a, &subagent_tools[i]);
}