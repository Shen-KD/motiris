/* repl.c - interactive mode: multi-turn chat with line editing,
 * history, streaming output and local commands (/help /new /tools...).
 *
 * Entered automatically when running without -p on a terminal, or with
 * `motiris -i`. One agent instance is reused across turns so the model
 * keeps full context of the conversation.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linenoise.h>

#define HIST_ENV "MOTIRIS_HISTFILE"

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

static int handle_command(MotirisAgent *a, const char *line) {
  if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) return 1;
  if (!strcmp(line, "/new")) {
    /* fresh session: drop all history messages */
    cJSON *msgs = motiris_messages(a);
    cJSON_Delete(msgs->child);
    msgs->child = msgs->prev = msgs->next = NULL;
    printf("[new session]\n");
    return 0;
  }
  if (!strcmp(line, "/help")) {
    printf("commands: /quit /exit, /new (reset session), /tools, /help\n");
    printf("multi-line input: end a line with two backslashes \\\\\n");
    return 0;
  }
  if (!strcmp(line, "/tools")) {
    printf("built-in plugin tool list is provided to the model each turn;\n");
    printf("runtime registry: see src/tools.c and --no-tools\n");
    return 0;
  }
  return -1; /* not a command */
}

int motiris_repl(MotirisAgent *a) {
  motiris_set_stream(a, 1);
  motiris_set_stream_cb(a, delta_print, NULL);
  int color = isatty(1) ? 1 : 0;

  char hist[1024];
  expand_home(hist, sizeof hist, hist_path());
  linenoiseHistoryLoad(hist);

  printf("motiris interactive — /help for commands, ctrl-d to quit\n");
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

    if (color) printf("\033[36m"); /* cyan while streaming */
    int rc = motiris_run(a);
    if (color) printf("\033[0m\n");
    if (rc) fprintf(stderr, "motiris: %s\n", motiris_last_error(a));
  }
  printf("\nbye\n");
  return 0;
}