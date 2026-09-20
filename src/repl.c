/* repl.c - interactive mode: multi-turn chat with line editing,
 * history, streaming output and local commands (/help /new /tools...).
 *
 * Entered automatically when running without -p on a terminal, or with
 * `motiris -i`. One agent instance is reused across turns so the model
 * keeps full context of the conversation.
 *
 * UI: tab completes /-commands (linenoise completion), typing `/<pre>`
 * shows a grey hint of the full command, model replies stream in cyan,
 * command output and errors get their own colors. The prompt itself
 * stays plain text on purpose: linenoise computes cursor width from
 * strlen(prompt), ANSI escapes would scramble it.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linenoise.h>

#define HIST_ENV "MOTIRIS_HISTFILE"

#define C_RESET "\033[0m"
#define C_CYAN  "\033[36m"
#define C_GREEN "\033[1;32m"
#define C_RED   "\033[31m"
#define C_DIM   "\033[2m"
#define C_BOLD  "\033[1m"

static const char *const CMDS[] = { "/help", "/new", "/tools", "/exit", "/quit" };
static const char *const CMDS_DESC[] = {
  "this help",
  "reset session context",
  "list tool information",
  "leave the repl",
  "leave the repl",
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

static void delta_print(const char *text, void *ud) {
  (void)ud;
  fputs(text, stdout);
  fflush(stdout);
}

static const char *hist_path(void) {
  const char *h = getenv(HIST_ENV);
  return (h && *h) ? h : "~/.motiris_history";
}

static void expand_home(char *dst, size_t n, const char *src) {
  if (src[0] == '~' && src[1] == '/') {
    const char *home = getenv("HOME");
    if (home) { snprintf(dst, n, "%s%s", home, src + 1); return; }
  }
  snprintf(dst, n, "%s", src);
}

/* ---------------- tab completion for /-commands ---------------- */
static void repl_completion(const char *buf, linenoiseCompletions *lc) {
  size_t blen = strlen(buf);
  if (blen == 0 || buf[0] != '/') return;
  for (size_t i = 0; i < NCMDS; i++)
    if (!strncmp(buf, CMDS[i], blen))
      linenoiseAddCompletion(lc, CMDS[i]);
}

/* grey hint under the line while typing a /-command prefix */
static char hint_buf[64];
static char *repl_hints(const char *line, int *color, int *bold) {
  size_t blen = strlen(line);
  if (blen < 2 || line[0] != '/') return NULL;
  for (size_t i = 0; i < NCMDS; i++) {
    if (!strncmp(line, CMDS[i], blen)) {
      if (blen == strlen(CMDS[i])) return NULL; /* fully typed: no hint */
      snprintf(hint_buf, sizeof hint_buf, "  %s %s", CMDS[i],
               CMDS_DESC[i]);
      *color = 90;
      *bold = 0;
      return hint_buf;
    }
  }
  return NULL;
}

static void print_help(void) {
  printf(C_BOLD "commands" C_RESET " (tab completes, hints while typing):\n");
  for (size_t i = 0; i < NCMDS; i++)
    printf("  " C_GREEN "%-8s" C_RESET " %s\n", CMDS[i], CMDS_DESC[i]);
  printf("  multi-line input: end a line with two backslashes " C_DIM "\\\\" C_RESET "\n");
}

static int handle_command(MotirisAgent *a, const char *line) {
  if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) return 1;
  if (!strcmp(line, "/new")) {
    /* fresh session: drop all history messages */
    cJSON *msgs = motiris_messages(a);
    cJSON_Delete(msgs->child);
    msgs->child = msgs->prev = msgs->next = NULL;
    printf(C_DIM "[context cleared] new session\n" C_RESET);
    return 0;
  }
  if (!strcmp(line, "/help")) { print_help(); return 0; }
  if (!strcmp(line, "/tools")) {
    printf("the tool list (shell, time, file/web/memory/skills/subagent,\n");
    printf("browser, MCP <server>:<tool>) is provided to the model each turn;\n");
    printf("disable with --no-tools, or per-tool allow/deny in config.json\n");
    return 0;
  }
  return -1; /* not a command */
}

int motiris_repl(MotirisAgent *a) {
  motiris_set_stream(a, 1);
  motiris_set_stream_cb(a, delta_print, NULL);
  int color = isatty(1) ? 1 : 0;

  linenoiseSetCompletionCallback(repl_completion);
  linenoiseSetHintsCallback(repl_hints);

  char hist[1024];
  expand_home(hist, sizeof hist, hist_path());
  linenoiseHistoryLoad(hist);

  printf(C_CYAN "motiris" C_RESET " — " C_BOLD "iris as a mote" C_RESET
         ", one tiny binary\n");
  printf("type " C_GREEN "/help" C_RESET " for commands, tab completes "
         C_DIM "//-commands" C_RESET ", ctrl-d to quit\n");
  fflush(stdout);

  char *line;
  while ((line = linenoise("motiris> ")) != NULL) {
    if (!*line) { free(line); continue; }
    linenoiseHistoryAdd(line);
    linenoiseHistorySave(hist);

    int cmd = handle_command(a, line);
    if (cmd == 1) { free(line); break; }
    if (cmd == 0) { free(line); continue; }

    motiris_add_user(a, line);
    free(line);

    printf(C_DIM "→ you" C_RESET "\n");
    if (color) printf(C_CYAN);
    int rc = motiris_run(a);
    if (color) printf(C_RESET "\n");
    if (rc) fprintf(stderr, C_RED "motiris: %s" C_RESET "\n",
                    motiris_last_error(a));
    printf("\n");
  }
  printf(C_DIM "bye" C_RESET "\n");
  return 0;
}