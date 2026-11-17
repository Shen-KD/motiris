/* subagent.c - subagent tool (issue #9).
 *
 * Spawns a fresh `motiris -p <task> --no-tools` child process and
 * returns its output. The child gets zero tools, so a delegated task
 * can never escape through the parent's shell/filesystem surface; the
 * parent keeps its own context untouched. Subagent env is inherited;
 * override binary with MOTIRIS_BIN.
 */
#include "motiris.h"
#include "childproc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUB_OUT_CAP (32 * 1024)
#define SUB_DEFAULT_TIMEOUT_S 120L

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
  if (!bin) { cJSON_Delete(args); return err_json("subagent path allocation failed"); }
  char *const child_argv[] = {
    bin, "-p", (char *)task, "--no-tools", "--max-steps", "3", NULL
  };
  ChildProcResult child;
  childproc_run("subagent", child_argv, "MOTIRIS_SUBAGENT_TIMEOUT_S",
                SUB_DEFAULT_TIMEOUT_S, SUB_OUT_CAP, &child);
  cJSON_Delete(args);
  free(bin);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "output", child.output ? child.output : "");
  cJSON_AddNumberToObject(r, "exit_code", child.exit_code);
  cJSON_AddNumberToObject(r, "signal", child.signal_number);
  cJSON_AddBoolToObject(r, "timed_out", child.timed_out);
  cJSON_AddBoolToObject(r, "truncated", child.truncated);
  if (child.error[0]) cJSON_AddStringToObject(r, "error", child.error);
  childproc_free(&child);
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
