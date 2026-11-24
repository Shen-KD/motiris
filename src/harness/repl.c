/* repl.c - interactive mode: multi-turn chat with line editing,
 * history, streaming output and local commands (/help /new /tools...).
 *
 * Entered automatically when running without -p on a terminal, or with
 * `motiris -i`. One agent instance is reused across turns so the model
 * keeps full context of the conversation.
 *
 * UI: typing `/` opens a live command menu, typing `/<pre>` filters it
 * and shows a grey hint; tab still completes commands, replies stream in cyan,
 * command output and errors get their own colors. The prompt itself
 * stays plain text on purpose: linenoise computes cursor width from
 * strlen(prompt), ANSI escapes would scramble it.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>

#include <linenoise.h>

#ifdef MOTIRIS_TTS_TOOLS
#include "tts.h"
#endif

#define HIST_ENV "MOTIRIS_HISTFILE"

#define C_RESET "\033[0m"
#define C_CYAN  "\033[36m"
#define C_GREEN "\033[1;32m"
#define C_RED   "\033[31m"
#define C_DIM   "\033[2m"
#define C_BOLD  "\033[1m"
#define C_REVERSE "\033[7m"
#define C_YELLOW "\033[33m"

static const char *const CMDS[] = {
  "/help", "/new", "/tools", "/sessions", "/load", "/save", "/resume",
  "/skills", "/thinking", "/reasoning", "/approval", "/config",
  "/hotkey", "/exit"
};
static const char *const CMDS_DESC[] = {
  "this help",
  "reset session context",
  "list tool information",
  "review past session logs [TERM]",
  "restore latest or selected session [UUID|FILE]",
  "save the current automatic session now",
  "restore a full-turn snapshot FILE",
  "list available skills [TERM]",
  "show or hide provider reasoning [on|off]",
  "set reasoning effort [off|low|medium|high]",
  "show or set tool approval [off|ask]",
  "configure this session interactively",
  "show interactive key bindings",
  "leave the repl",
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

/* Automatic full-turn checkpoint id (tty only). */
static char *sess_id = NULL;
/* Lock-free C11 atomics are valid for both signal flags and worker visibility;
 * volatile sig_atomic_t alone does not synchronize the two REPL threads. */
static _Atomic int generation_interrupted;
static _Atomic int terminal_hangup;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "REPL signal handlers require lock-free int atomics");

static void generation_sigint(int sig) {
  (void)sig;
  atomic_store_explicit(&generation_interrupted, 1, memory_order_relaxed);
}

static void terminal_sighup(int sig) {
  (void)sig;
  atomic_store_explicit(&terminal_hangup, 1, memory_order_relaxed);
  atomic_store_explicit(&generation_interrupted, 1, memory_order_relaxed);
}

static int generation_cancelled(void *ud) {
  (void)ud;
  return atomic_load_explicit(&generation_interrupted, memory_order_relaxed);
}

/* Raw linenoise editing handles Ctrl+C itself. During a model run the
 * terminal is back in canonical mode, so SIGINT becomes a scoped cancel flag
 * instead of terminating the whole REPL. */
static int run_interruptible(MotirisAgent *a) {
  struct sigaction action = {0}, previous;
  action.sa_handler = generation_sigint;
  sigemptyset(&action.sa_mask);
  atomic_store_explicit(&generation_interrupted, 0, memory_order_relaxed);
  if (sigaction(SIGINT, &action, &previous) != 0) return motiris_run(a);
  motiris_set_cancel_cb(a, generation_cancelled, NULL);
  int rc = motiris_run(a);
  motiris_set_cancel_cb(a, NULL, NULL);
  sigaction(SIGINT, &previous, NULL);
  return rc;
}

typedef struct {
  MotirisAgent *agent;
  int reasoning_active;
  char *session_id;
} ReplState;

static void delta_print(const char *text, void *ud) {
  ReplState *state = ud;
  if (state->reasoning_active) {
    printf(C_RESET "\n");
    state->reasoning_active = 0;
  }
  fputs(text, stdout);
  fflush(stdout);
}

static void reasoning_print(const char *text, void *ud) {
  ReplState *state = ud;
  if (!motiris_thinking_visible(state->agent)) return;
  if (!state->reasoning_active) {
    printf(C_DIM "\n[thinking]\n" C_RESET);
    state->reasoning_active = 1;
  }
  printf(C_DIM "%s" C_RESET, text);
  fflush(stdout);
}

/* copy at most n-1 chars, whitespace collapsed */
static void clip(char *dst, size_t n, const char *src, size_t maxlen) {
  size_t i = 0, o = 0;
  int prev_sp = 0;
  while (src[i] && o + 1 < n && o < maxlen) {
    unsigned char c = (unsigned char)src[i++];
    if (c < 32 || c == 127) c = ' ';
    if (c == ' ') {
      if (prev_sp) continue;
      prev_sp = 1;
    } else prev_sp = 0;
    dst[o++] = c;
  }
  if (o >= maxlen && o + 1 < n) { dst[o++] = '.', dst[o++] = '.'; }
  dst[o] = '\0';
}

/* tool-call observer: yellow ⚙ line; full tool objects enter the snapshot */
static void repl_tool_hook(const char *name, const char *args,
                           const char *result, void *ud) {
  (void)ud;
  char abuf[80], rbuf[96];
  clip(abuf, sizeof abuf, args ? args : "{}", 60);
  int is_err = result && strstr(result, "\"error\"");
  if (is_err) {
    clip(rbuf, sizeof rbuf, result, 72);
    printf("  " C_YELLOW "⚙ %s" C_RESET "(%s) " C_RED "✗ %s" C_RESET "\n",
           name, abuf, rbuf);
  } else {
    clip(rbuf, sizeof rbuf, result, 72);
    printf("  " C_YELLOW "⚙ %s" C_RESET "(%s) " C_DIM "→ %s" C_RESET "\n",
           name, abuf, rbuf);
  }
}

/* ---------------- shared interactive cards ---------------- */
typedef struct {
  struct termios termios;
  struct sigaction sigint;
  int active;
} CardTerminal;

static _Atomic int card_interrupted;

static void card_sigint(int sig) {
  (void)sig;
  atomic_store_explicit(&card_interrupted, 1, memory_order_relaxed);
}

static int card_terminal_start(CardTerminal *terminal) {
  memset(terminal, 0, sizeof *terminal);
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
      tcgetattr(STDIN_FILENO, &terminal->termios)) return 0;
  struct termios raw = terminal->termios;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
  raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1; /* a lone Escape times out; arrows arrive together */
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) return 0;
  struct sigaction action = {0};
  action.sa_handler = card_sigint;
  sigemptyset(&action.sa_mask);
  atomic_store_explicit(&card_interrupted, 0, memory_order_relaxed);
  if (sigaction(SIGINT, &action, &terminal->sigint)) {
    tcsetattr(STDIN_FILENO, TCSANOW, &terminal->termios);
    return 0;
  }
  terminal->active = 1;
  printf("\033[?25l");
  return 1;
}

static void card_terminal_stop(CardTerminal *terminal) {
  if (!terminal->active) return;
  printf("\033[?25h" C_RESET "\n");
  fflush(stdout);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal->termios);
  sigaction(SIGINT, &terminal->sigint, NULL);
  terminal->active = 0;
}

/* u/d/l/r navigate, Enter selects, q/Escape/Ctrl+C closes. */
static int card_read_key(void) {
  unsigned char c;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  if (n < 0 && errno == EINTR)
    return atomic_load_explicit(&card_interrupted, memory_order_relaxed) ? 'q' : 0;
  if (n < 0) return 'q';
  if (n == 0) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&pfd, 1, 0) > 0 &&
        (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) return 'q';
  }
  if (n <= 0)
    return atomic_load_explicit(&card_interrupted, memory_order_relaxed) ? 'q' : 0;
  if (c == 3 || c == 'q' || c == 'Q') return 'q';
  if (c == '\r' || c == '\n') return '\n';
  if (c == 's' || c == 'S') return 's';
  if (c != 27) return (int)c;
  unsigned char seq[2];
  if (read(STDIN_FILENO, seq, 1) != 1) return 'q';
  if (seq[0] != '[' && seq[0] != 'O') return 'q';
  if (read(STDIN_FILENO, seq + 1, 1) != 1) return 'q';
  if (seq[1] == 'A') return 'u';
  if (seq[1] == 'B') return 'd';
  if (seq[1] == 'C') return 'r';
  if (seq[1] == 'D') return 'l';
  return 0;
}

static void card_row(int selected, const char *label, const char *value) {
  printf(selected ? C_REVERSE " > %-18s %-24s " C_RESET "\r\n"
                  : "   %-18s %-24s\r\n", label, value);
}

static void draw_config_card(MotirisAgent *a, int selected) {
  const char *effort = motiris_reasoning_effort(a);
  char steps[32];
  snprintf(steps, sizeof steps, "%d%s", motiris_max_steps(a),
           motiris_max_steps(a) ? "" : " (unlimited)");
  printf("\033[2J\033[H" C_BOLD "Session configuration" C_RESET "\r\n"
         "Enter: change/select  arrows: navigate  s: save  q: close\r\n\r\n");
  card_row(selected == 0, "stream:", motiris_stream_enabled(a) ? "on" : "off");
  card_row(selected == 1, "approval:", motiris_approval_required(a) ? "ask" : "off");
  card_row(selected == 2, "max_steps:", steps);
  card_row(selected == 3, "thinking:",
           motiris_thinking_visible(a) ? "on" : "off");
  card_row(selected == 4, "reasoning:", effort && *effort ? effort : "provider");
  card_row(selected == 5, "workspace_guard:",
           motiris_workspace_guard_enabled(a) ? "on" : "off (any OS path)");
  card_row(selected == 6, "save session now", "UUID trace + index");
  card_row(selected == 7, "restore latest", "requires empty context");
  card_row(selected == 8, "save changes", "to config.json");
  card_row(selected == 9, "close", "keep session values");
  fflush(stdout);
}

static void save_config_card(MotirisAgent *a) {
  char steps[32], effort[16];
  snprintf(steps, sizeof steps, "%d", motiris_max_steps(a));
  const char *current = motiris_reasoning_effort(a);
  snprintf(effort, sizeof effort, "%s", current && *current ? current : "off");
  int failed = 0;
  failed += motiris_set_config("stream",
                               motiris_stream_enabled(a) ? "true" : "false") != 0;
  failed += motiris_set_config("approval",
                               motiris_approval_required(a) ? "ask" : "off") != 0;
  failed += motiris_set_config("max_steps", steps) != 0;
  failed += motiris_set_config("show_thinking",
                               motiris_thinking_visible(a) ? "true" : "false") != 0;
  failed += motiris_set_config("workspace_guard",
                               motiris_workspace_guard_enabled(a) ? "true" : "false") != 0;
  failed += motiris_set_config("reasoning_effort", effort) != 0;
  if (failed) printf(C_RED "configuration: %d values could not be saved" C_RESET "\n",
                     failed);
  else printf(C_DIM "configuration saved" C_RESET "\n");
}

static void config_card(ReplState *state) {
  MotirisAgent *a = state->agent;
  static const int step_choices[] = { 0, 5, 10, 25, 50, 100 };
  static const char *const efforts[] = { "", "low", "medium", "high" };
  CardTerminal terminal;
  if (!card_terminal_start(&terminal)) return;
  int selected = 0, save = 0, session_action = 0, done = 0;
  while (!done) {
    draw_config_card(a, selected);
    int key = card_read_key();
    if (key == 'q') break;
    if (key == 'u' || key == 'l') {
      selected = (selected + 9) % 10;
      continue;
    } else if (key == 'd' || key == 'r') {
      selected = (selected + 1) % 10;
      continue;
    } else if (key == 's') { save = 1; break; }
    else if (key != '\n') continue;

    if (selected == 0)
      motiris_set_stream(a, !motiris_stream_enabled(a));
    else if (selected == 1)
      motiris_set_approval_required(a, !motiris_approval_required(a));
    else if (selected == 2) {
      int current = 0;
      for (size_t i = 0; i < sizeof step_choices / sizeof step_choices[0]; i++)
        if (step_choices[i] == motiris_max_steps(a)) current = (int)i;
      motiris_set_max_steps(a, step_choices[(current + 1) % 6]);
    } else if (selected == 3)
      motiris_set_thinking_visible(a, !motiris_thinking_visible(a));
    else if (selected == 4) {
      const char *current = motiris_reasoning_effort(a);
      int index = 0;
      for (int i = 0; i < 4; i++)
        if (!strcmp(current, efforts[i])) { index = i; break; }
      motiris_set_reasoning_effort(a, efforts[(index + 1) % 4]);
    } else if (selected == 5)
      motiris_set_workspace_guard(a, !motiris_workspace_guard_enabled(a));
    else if (selected == 6) { session_action = 1; done = 1; }
    else if (selected == 7) { session_action = 2; done = 1; }
    else if (selected == 8) { save = 1; done = 1; }
    else if (selected == 9) done = 1;
  }
  card_terminal_stop(&terminal);
  if (save) save_config_card(a);
  if (session_action == 1) {
    const char *error = NULL;
    if (motiris_session_auto_save(a, state->session_id, "repl", NULL, &error))
      printf(C_RED "session not saved: %s" C_RESET "\n",
             error ? error : "snapshot failed");
    else printf(C_DIM "session %s saved" C_RESET "\n", state->session_id);
  } else if (session_action == 2) {
    if (motiris_messages(a)->child) {
      printf(C_YELLOW "use /new before restoring the latest session" C_RESET "\n");
    } else {
      const char *error = NULL;
      char *uuid = NULL;
      size_t loaded = 0;
      int rc = motiris_session_auto_restore(a, "repl", NULL, NULL,
                                            &uuid, &loaded, &error);
      if (rc) printf(C_RED "session not restored: %s" C_RESET "\n",
                     error ? error : "no automatic session");
      else {
        free(state->session_id);
        state->session_id = uuid;
        printf(C_DIM "restored %zu messages from %s" C_RESET "\n", loaded, uuid);
      }
    }
  }
}

static void draw_approval_card(const char *name, const char *args, int selected) {
  static const char *const choices[] = {
    "allow once", "allow for session", "deny"
  };
  char safe_name[96], safe_args[160];
  clip(safe_name, sizeof safe_name, name ? name : "(unknown)", 70);
  clip(safe_args, sizeof safe_args, args ? args : "{}", 120);
  printf("\033[2J\033[H" C_BOLD "Tool approval" C_RESET "\r\n"
         "tool: %s\r\nargs: %s\r\n\r\n", safe_name, safe_args);
  for (int i = 0; i < 3; i++)
    card_row(selected == i, choices[i], "");
  printf("\r\nEnter: select  arrows: navigate  q/Esc: deny\r\n");
  fflush(stdout);
}

static MotirisApprovalDecision approval_card(const char *name,
                                              const char *args, void *ud) {
  (void)ud;
  CardTerminal terminal;
  if (!card_terminal_start(&terminal)) return MOTIRIS_APPROVAL_DENY;
  int selected = 0;
  MotirisApprovalDecision decision = MOTIRIS_APPROVAL_DENY;
  for (;;) {
    draw_approval_card(name, args, selected);
    int key = card_read_key();
    if (key == 'q') break;
    if (key == 'u' || key == 'l') selected = (selected + 2) % 3;
    else if (key == 'd' || key == 'r') selected = (selected + 1) % 3;
    else if (key == '\n') {
      decision = selected == 0 ? MOTIRIS_APPROVAL_ALLOW_ONCE :
                 selected == 1 ? MOTIRIS_APPROVAL_ALLOW_SESSION :
                                 MOTIRIS_APPROVAL_DENY;
      break;
    }
  }
  card_terminal_stop(&terminal);
  return decision;
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

/* Live newline-separated menu; no Tab keypress is required. */
static char menu_buf[1024];
static char *repl_menu(const char *line, int selected) {
  size_t blen = strlen(line);
  if (!blen || line[0] != '/' || strchr(line, ' ')) return NULL;

  size_t used = 0;
  int matches = 0;
  menu_buf[0] = '\0';
  for (size_t i = 0; i < NCMDS; i++) {
    if (strncmp(line, CMDS[i], blen)) continue;
    char row[160];
    int n = matches == selected
          ? snprintf(row, sizeof row, C_REVERSE "  %-8s %s" C_RESET,
                     CMDS[i], CMDS_DESC[i])
          : snprintf(row, sizeof row, "  %-8s %s", CMDS[i], CMDS_DESC[i]);
    if (n < 0 || (size_t)n >= sizeof row) continue;
    size_t need = (size_t)n + (matches ? 1 : 0);
    if (used + need >= sizeof menu_buf) break;
    if (matches) menu_buf[used++] = '\n';
    memcpy(menu_buf + used, row, (size_t)n + 1);
    used += (size_t)n;
    matches++;
  }
  if (!matches)
    snprintf(menu_buf, sizeof menu_buf, "  no matching commands (/help)");
  return menu_buf;
}

static char menu_select_buf[32];
static char *repl_menu_select(const char *line, int selected) {
  size_t blen = strlen(line);
  if (!blen || line[0] != '/' || strchr(line, ' ')) return NULL;
  int match = 0;
  for (size_t i = 0; i < NCMDS; i++) {
    if (strncmp(line, CMDS[i], blen)) continue;
    if (match++ == selected) {
      snprintf(menu_select_buf, sizeof menu_select_buf, "%s", CMDS[i]);
      return menu_select_buf;
    }
  }
  return NULL;
}

static void print_help(void) {
  printf(C_BOLD "commands" C_RESET " (/ shows a live list, tab completes):\n");
  for (size_t i = 0; i < NCMDS; i++)
    printf("  " C_GREEN "%-8s" C_RESET " %s\n", CMDS[i], CMDS_DESC[i]);
  printf("  multi-line input: end a line with two backslashes " C_DIM "\\\\" C_RESET "\n");
}

static void print_hotkeys(void) {
  printf(C_BOLD "interactive keys" C_RESET "\n");
  printf("  " C_GREEN "↑ / ↓" C_RESET
         "      select a command in the / menu; browse history otherwise\n");
  printf("  " C_GREEN "enter" C_RESET
         "      accept input; queue it during a reply, run it when idle\n");
  printf("  " C_GREEN "tab" C_RESET
         "        complete a /-command\n");
  printf("  " C_GREEN "ctrl+c / esc" C_RESET
         " cancel a running reply; keep the draft and queued input\n");
  printf("  " C_GREEN "ctrl+d" C_RESET
         "     quit at an empty prompt\n");
  printf("  " C_GREEN "ctrl+u" C_RESET
         "     clear the current input\n");
  printf("  " C_GREEN "ctrl+l" C_RESET
         "     clear the screen\n");
}

static int handle_command(MotirisAgent *a, ReplState *state,
                          const char *line) {
  if (!strcmp(line, "/exit")) return 1;
  if (!strcmp(line, "/hotkey")) { print_hotkeys(); return 0; }
  if (!strcmp(line, "/new")) {
    /* fresh session: drop history and tool-specific session grants */
    cJSON *msgs = motiris_messages(a);
    cJSON_Delete(msgs->child);
    msgs->child = msgs->prev = msgs->next = NULL;
    motiris_clear_approval_grants(a);
    free(state->session_id);
    state->session_id = motiris_session_auto_id();
    printf(C_DIM "[context cleared] new session %s\n" C_RESET,
           state->session_id ? state->session_id : "(volatile)");
    return 0;
  }
  if (!strcmp(line, "/help")) { print_help(); return 0; }
  if (!strcmp(line, "/thinking") || !strncmp(line, "/thinking ", 10)) {
    const char *arg = strchr(line, ' ');
    if (arg) {
      arg++;
      if (!strcmp(arg, "on")) motiris_set_thinking_visible(a, 1);
      else if (!strcmp(arg, "off")) motiris_set_thinking_visible(a, 0);
      else {
        printf(C_RED "usage: /thinking [on|off]\n" C_RESET);
        return 0;
      }
    }
    printf("thinking: %s\n", motiris_thinking_visible(a) ? "on" : "off");
    return 0;
  }
  if (!strcmp(line, "/reasoning") || !strncmp(line, "/reasoning ", 11)) {
    const char *arg = strchr(line, ' ');
    if (arg) {
      arg++;
      if (motiris_set_reasoning_effort(a, arg)) {
        printf(C_RED "usage: /reasoning [off|low|medium|high]\n" C_RESET);
        return 0;
      }
    }
    const char *effort = motiris_reasoning_effort(a);
    printf("reasoning: %s\n", effort && *effort ? effort : "provider");
    return 0;
  }
  if (!strcmp(line, "/approval") || !strncmp(line, "/approval ", 10)) {
    const char *arg = strchr(line, ' ');
    if (arg) {
      arg++;
      if (!strcmp(arg, "off")) motiris_set_approval_required(a, 0);
      else if (!strcmp(arg, "ask")) motiris_set_approval_required(a, 1);
      else {
        printf(C_RED "usage: /approval [off|ask]\n" C_RESET);
        return 0;
      }
    }
    printf("approval: %s%s\n", motiris_approval_required(a) ? "ask" : "off",
           motiris_approval_required(a) ? "" : " (unrestricted)");
    return 0;
  }
  if (!strcmp(line, "/config")) {
    const char *effort = motiris_reasoning_effort(a);
    /* Pipe mode must never enter a raw-terminal interaction. */
    printf("stream: %s\napproval: %s\nmax_steps: %d\nthinking: %s\nreasoning: %s\nworkspace_guard: %s\n",
           motiris_stream_enabled(a) ? "on" : "off",
           motiris_approval_required(a) ? "ask" : "off",
           motiris_max_steps(a),
           motiris_thinking_visible(a) ? "on" : "off",
           effort && *effort ? effort : "provider",
           motiris_workspace_guard_enabled(a) ? "on" : "off");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      printf(C_DIM "interactive terminal required to edit config\n" C_RESET);
    else
      config_card(state);
    return 0;
  }
  if (!strcmp(line, "/tools")) {
    printf("the tool list (shell, time, file/web/memory/skills/subagent,\n");
    printf("browser, MCP <server>:<tool>) is provided to the model each turn;\n");
    printf("disable with --no-tools, or per-tool allow/deny in config.json\n");
    return 0;
  }
  if (!strcmp(line, "/sessions") || !strncmp(line, "/sessions ", 10)) {
    const char *term = strchr(line, ' ');
    if (term) term++;
    printf(C_DIM "== automatic sessions ==" C_RESET "\n");
    motiris_session_auto_list(NULL, term);
    printf(C_DIM "== repl sessions ==" C_RESET "\n");
    motiris_session_list("sessions", term);
    printf(C_DIM "== gateway sessions ==" C_RESET "\n");
    motiris_session_list("gateway", term);
    return 0;
  }
  if (!strcmp(line, "/save")) {
    const char *error = NULL;
    if (motiris_session_auto_save(a, state->session_id, "repl", NULL, &error))
      printf(C_RED "session not saved: %s\n" C_RESET,
             error ? error : "snapshot failed");
    else
      printf(C_DIM "[saved session %s]\n" C_RESET, state->session_id);
    return 0;
  }
  if (!strcmp(line, "/load") || !strncmp(line, "/load ", 6)) {
    if (motiris_messages(a)->child) {
      printf(C_RED "session restore requires an empty context (use /new first)\n"
             C_RESET);
      return 0;
    }
    const char *selector = strchr(line, ' ');
    if (selector) selector++;
    const char *error = NULL;
    size_t loaded = 0;
    char *uuid = NULL;
    int rc;
    if (selector && (strchr(selector, '/') || strstr(selector, ".json"))) {
      rc = motiris_session_load(a, selector, NULL, &loaded, &error);
      if (!rc) uuid = motiris_session_auto_id();
    } else {
      rc = motiris_session_auto_restore(a, "repl", NULL, selector,
                                        &uuid, &loaded, &error);
      if (rc == 1) error = "no matching automatic session";
    }
    if (rc) {
      printf(C_RED "cannot restore session: %s\n" C_RESET,
             error ? error : "invalid snapshot");
      free(uuid);
      return 0;
    }
    free(state->session_id);
    state->session_id = uuid;
    printf(C_DIM "[loaded %zu messages from session %s]\n" C_RESET, loaded,
           state->session_id ? state->session_id : "(unknown)");
    return 0;
  }
  if (!strcmp(line, "/resume") || !strncmp(line, "/resume ", 8)) {
    const char *file = strchr(line, ' ');
    if (!file || !*++file) {
      printf(C_RED "usage: /resume FILE\n" C_RESET);
      return 0;
    }
    const char *error = NULL;
    size_t loaded = 0;
    if (motiris_session_load(a, file, NULL, &loaded, &error)) {
      printf(C_RED "cannot resume %s: %s%s\n" C_RESET, file,
             error ? error : "invalid snapshot",
             motiris_messages(a)->child ? " (use /new first)" : "");
      return 0;
    }
    printf(C_DIM "[loaded %zu messages from %s]\n" C_RESET, loaded, file);
    return 0;
  }
  if (!strcmp(line, "/skills") || !strncmp(line, "/skills ", 8)) {
    const char *term = strchr(line, ' ');
    if (term) term++;
    int n = motiris_skill_count();
    if (n <= 0) {
      printf(C_DIM "no skills (use --skill-dir DIR)\n" C_RESET);
      return 0;
    }
    size_t tlen = term ? strlen(term) : 0;
    int shown = 0;
    for (int i = 0; i < n; i++) {
      const char *nm = motiris_skill_name(i);
      const char *ds = motiris_skill_desc(i);
      if (tlen && !strcasestr(nm, term) && !strcasestr(ds ? ds : "", term))
        continue;
      printf("  " C_GREEN "%-24s" C_RESET C_DIM "%s" C_RESET "\n",
             nm, ds ? ds : "");
      shown = 1;
    }
    if (!shown) printf(C_DIM "no skills match\n" C_RESET);
    return 0;
  }
  if (line[0] == '/') {
    printf(C_RED "unknown command: %s (use /help)\n" C_RESET, line);
    return 0;
  }
  return -1; /* not a command */
}

/* startup banner: model / skills / tools at a glance */
static void print_banner(MotirisAgent *a) {
  printf(C_DIM "─────────────────────────────────────────────────────────"
         "──────────\n" C_RESET);
  printf(C_CYAN "motiris" C_RESET " — " C_BOLD "iris as a mote" C_RESET
         ", one tiny binary\n");

  const char *m = motiris_model(a);
  printf("  " C_BOLD C_GREEN "%-7s" C_RESET " " C_CYAN "%s" C_RESET "\n",
         "model", m && *m ? m : "(none)");

  int nsk = motiris_skill_count();
  if (nsk > 0) {
    printf("  " C_BOLD C_GREEN "%-7s" C_RESET " " C_GREEN "%d" C_RESET ": ",
           "skills", nsk);
    for (int i = 0; i < nsk; i++)
      printf("%s%s", i ? ", " : "", motiris_skill_name(i));
    printf("\n");
  } else {
    printf("  " C_BOLD C_GREEN "%-7s" C_RESET " none " C_DIM
           "(use --skill-dir DIR)" C_RESET "\n", "skills");
  }

  if (!motiris_tools_enabled(a)) {
    printf("  " C_BOLD C_GREEN "%-7s" C_RESET " none " C_DIM
           "(--no-tools)" C_RESET "\n", "tools");
  } else {
    int nt = motiris_tool_count(a);
    if (nt <= 0) {
      printf("  " C_BOLD C_GREEN "%-7s" C_RESET " none\n", "tools");
    } else {
      printf("  " C_BOLD C_GREEN "%-7s" C_RESET " " C_YELLOW "%d" C_RESET
             ": ", "tools", nt);
      int col = 11;
      for (int i = 0; i < nt; i++) {
        const char *nm = motiris_tool_name(a, i);
        int ln = (int)strlen(nm) + (i ? 2 : 0);
        if (col + ln > 78) { printf("\n             "); col = 13; }
        printf("%s%s", i ? ", " : "", nm);
        col += ln;
      }
      printf("\n");
    }
  }

  printf(C_DIM "─────────────────────────────────────────────────────────"
         "──────────\n" C_RESET);
  printf("type " C_GREEN "/help" C_RESET " for commands, "
         C_GREEN "/" C_RESET " lists commands, ctrl-c cancels a reply, "
         "ctrl-d to quit\n");
  fflush(stdout);
}

#ifdef MOTIRIS_TTS_TOOLS
/* Match goal-check's synthetic acknowledgement so it is not spoken. */
static int tts_done_reply(const char *s) {
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

static int tts_goal_prompt(const cJSON *message) {
  const char *role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "role"));
  const char *prompt = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "content"));
  return role && !strcmp(role, "user") && prompt
      && !strncmp(prompt, "Goal check: <", 13);
}

static void tts_new_reply(MotirisAgent *a) {
  if (!motiris_tts_enabled()) return;
  const cJSON *start = motiris_turn_start(a);
  const char *speech = NULL;
  int goal_confirm = 0;
  /* Restrict selection to this run; user-controlled goal-like text is not a
   * synthetic boundary. Never traverse cJSON's circular prev links. */
  for (const cJSON *m = start; m; m = m->next) {
    if (m != start && tts_goal_prompt(m)) goal_confirm = 1;
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "role"));
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "content"));
    if (role && !strcmp(role, "assistant")) {
      cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
      int confirmation_call = goal_confirm && cJSON_IsArray(calls) &&
                              cJSON_GetArraySize(calls) > 0;
      int synthetic_done = goal_confirm && content && tts_done_reply(content);
      /* Confirmation tools/DONE do not replace the real pre-goal answer. */
      if (!confirmation_call && !synthetic_done)
        speech = content && *content ? content : NULL;
    }
  }
  if (!speech) return;

  char diag[160];
  int rc = motiris_tts_speak(speech, diag, sizeof diag);
  if (rc < 0)
    printf(C_YELLOW "[tts: %s; text output preserved]" C_RESET "\n",
           diag[0] ? diag : "engine unavailable");
  else if (rc > 0)
    printf(C_DIM "[tts: %s]" C_RESET "\n", diag);
}
#endif

/* Traditional synchronous loop: pipes, unsupported terminals and raw-setup
 * fallback retain the original rendering, cancellation and checkpoint path. */
static int repl_sync_loop(MotirisAgent *a, ReplState *state,
                          const char *hist, int color) {
  char *line;
  while ((line = linenoise("motiris> ")) != NULL) {
    if (!*line) { free(line); continue; }
    linenoiseHistoryAdd(line);
    linenoiseHistorySave(hist);

    int cmd = handle_command(a, state, line);
    if (cmd == 1) { free(line); break; }
    if (cmd == 0) { free(line); continue; }

    motiris_add_user(a, line);
    free(line);

    printf(C_DIM "→ you" C_RESET "\n");
    if (color) printf(C_CYAN);
    state->reasoning_active = 0;
    long ti0 = motiris_tokens_in(a), to0 = motiris_tokens_out(a);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = run_interruptible(a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L
            + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    long di = motiris_tokens_in(a) - ti0;
    long dout = motiris_tokens_out(a) - to0;
    if (state->reasoning_active) {
      printf(C_RESET "\n");
      state->reasoning_active = 0;
    }
    if (color) printf(C_RESET "\n");
    if (rc == MOTIRIS_RUN_CANCELLED)
      fprintf(stderr, C_YELLOW "generation cancelled; enter a new instruction"
              C_RESET "\n");
    else if (rc) fprintf(stderr, C_RED "motiris: %s" C_RESET "\n",
                         motiris_last_error(a));
#ifdef MOTIRIS_TTS_TOOLS
    if (!rc) tts_new_reply(a);
#endif
#ifndef MOTIRIS_NO_PERSISTENCE
    {
      const char *error = NULL;
      if (motiris_session_auto_save(a, state->session_id, "repl", NULL, &error))
        fprintf(stderr, C_YELLOW "automatic session not saved: %s" C_RESET "\n",
                error ? error : "snapshot failed");
    }
#endif
    if (sess_id && !rc) {
      const char *error = NULL;
      if (motiris_session_checkpoint("sessions", sess_id, a, sess_id, &error))
        fprintf(stderr, C_YELLOW "session checkpoint not saved: %s" C_RESET "\n",
                error ? error : "snapshot failed");
    }
    if (di || dout)
      printf(C_DIM "[tokens " C_RESET C_CYAN "↑%ld" C_RESET C_DIM
             " " C_RESET C_GREEN "↓%ld" C_RESET C_DIM
             " · total %ld · %ldms · %s]" C_RESET "\n",
             di, dout, motiris_tokens_in(a) + motiris_tokens_out(a),
             ms, motiris_model(a));
    printf("\n");
  }
  return 0;
}

/* ---------------- incrementally edited tty session ----------------
 *
 * The main thread owns the terminal, editor, history, commands and all agent
 * access outside a run. A joinable worker owns the agent during motiris_run.
 * Its callbacks only enqueue bounded, deep-copied events and wake the main
 * thread through a nonblocking close-on-exec self-pipe. */
#define REPL_FIFO_MAX 32
#define REPL_FIFO_BYTES (64u * 1024u)
#define REPL_UI_EVENT_MAX 128
#define REPL_UI_EVENT_BYTES (256u * 1024u)
#define REPL_UI_CHUNK 4096u
#define REPL_EDIT_INITIAL 4096u

typedef enum {
  REPL_UI_DELTA,
  REPL_UI_REASONING,
  REPL_UI_FINAL,
  REPL_UI_TOOL,
  REPL_UI_DIAGNOSTIC
} ReplUiEventType;

typedef struct {
  ReplUiEventType type;
  char *text;
  size_t len;
} ReplUiEvent;

typedef struct {
  MotirisAgent *agent;
  int pipefd[2];
  pthread_mutex_t lock;
  pthread_cond_t space;
  pthread_cond_t approval_condition;
  ReplUiEvent events[REPL_UI_EVENT_MAX];
  size_t event_head;
  size_t event_count;
  size_t event_bytes;
  int closing;
  int finished;
  int run_rc;
  char *approval_name;
  char *approval_args;
  int approval_pending;
  int approval_done;
  MotirisApprovalDecision approval_decision;
} ReplAsync;

typedef struct {
  char *lines[REPL_FIFO_MAX];
  size_t lengths[REPL_FIFO_MAX];
  size_t head;
  size_t count;
  size_t bytes;
} ReplFifo;

typedef struct {
  MotirisAgent *agent;
  ReplState *state;
  ReplAsync async;
  ReplFifo fifo;
  struct linenoiseState editor;
  struct sigaction previous_sigint;
  struct sigaction previous_sighup;
  const char *hist;
  pthread_t worker;
  struct timespec started;
  long tokens_in;
  long tokens_out;
  int worker_active;
  int history_dirty;
  int sigint_installed;
  int sighup_installed;
  int hidden;
  int output_needs_newline;
  int exiting;
  int color;
} ReplTTY;

static int repl_async_fd_flags(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return -1;
  flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  return 0;
}

static int repl_async_init(ReplAsync *async, MotirisAgent *agent) {
  memset(async, 0, sizeof *async);
  async->agent = agent;
  async->pipefd[0] = async->pipefd[1] = -1;
  if (pipe(async->pipefd) != 0) return -1;
  if (repl_async_fd_flags(async->pipefd[0]) != 0 ||
      repl_async_fd_flags(async->pipefd[1]) != 0) {
    close(async->pipefd[0]);
    close(async->pipefd[1]);
    async->pipefd[0] = async->pipefd[1] = -1;
    return -1;
  }
  if (pthread_mutex_init(&async->lock, NULL) != 0) {
    close(async->pipefd[0]);
    close(async->pipefd[1]);
    async->pipefd[0] = async->pipefd[1] = -1;
    return -1;
  }
  if (pthread_cond_init(&async->space, NULL) != 0) {
    pthread_mutex_destroy(&async->lock);
    close(async->pipefd[0]);
    close(async->pipefd[1]);
    async->pipefd[0] = async->pipefd[1] = -1;
    return -1;
  }
  if (pthread_cond_init(&async->approval_condition, NULL) != 0) {
    pthread_cond_destroy(&async->space);
    pthread_mutex_destroy(&async->lock);
    close(async->pipefd[0]);
    close(async->pipefd[1]);
    async->pipefd[0] = async->pipefd[1] = -1;
    return -1;
  }
  return 0;
}

static void repl_async_wake(ReplAsync *async) {
  unsigned char byte = 0;
  ssize_t written;
  do {
    written = write(async->pipefd[1], &byte, 1);
  } while (written < 0 && errno == EINTR);
  /* A full pipe is already a pending wakeup. Other descriptor errors are
   * handled by the main loop's poll/HUP path. */
}

static void repl_async_destroy(ReplAsync *async) {
  while (async->event_count) {
    ReplUiEvent *event = &async->events[async->event_head];
    free(event->text);
    event->text = NULL;
    async->event_head = (async->event_head + 1) % REPL_UI_EVENT_MAX;
    async->event_count--;
  }
  free(async->approval_name);
  free(async->approval_args);
  async->approval_name = async->approval_args = NULL;
  pthread_cond_destroy(&async->approval_condition);
  pthread_cond_destroy(&async->space);
  pthread_mutex_destroy(&async->lock);
  if (async->pipefd[0] >= 0) close(async->pipefd[0]);
  if (async->pipefd[1] >= 0) close(async->pipefd[1]);
  async->pipefd[0] = async->pipefd[1] = -1;
}

static int repl_async_push(ReplAsync *async, ReplUiEventType type,
                           const char *text, size_t len) {
  char *copy = malloc(len + 1);
  if (!copy) return -1;
  memcpy(copy, text, len);
  copy[len] = '\0';

  pthread_mutex_lock(&async->lock);
  while (!async->closing &&
         (async->event_count == REPL_UI_EVENT_MAX ||
          len > REPL_UI_EVENT_BYTES - async->event_bytes))
    pthread_cond_wait(&async->space, &async->lock);
  if (async->closing) {
    pthread_mutex_unlock(&async->lock);
    free(copy);
    return -1;
  }
  size_t slot = (async->event_head + async->event_count) % REPL_UI_EVENT_MAX;
  async->events[slot].type = type;
  async->events[slot].text = copy;
  async->events[slot].len = len;
  async->event_count++;
  async->event_bytes += len;
  pthread_mutex_unlock(&async->lock);
  repl_async_wake(async);
  return 0;
}

static void repl_async_text(ReplAsync *async, ReplUiEventType type,
                            const char *text) {
  if (!text) return;
  size_t remaining = strlen(text);
  while (remaining) {
    size_t len = remaining > REPL_UI_CHUNK ? REPL_UI_CHUNK : remaining;
    if (repl_async_push(async, type, text, len) != 0) return;
    text += len;
    remaining -= len;
  }
}

static int repl_async_pop(ReplAsync *async, ReplUiEvent *event) {
  pthread_mutex_lock(&async->lock);
  if (!async->event_count) {
    pthread_mutex_unlock(&async->lock);
    return 0;
  }
  *event = async->events[async->event_head];
  async->events[async->event_head].text = NULL;
  async->events[async->event_head].len = 0;
  async->event_head = (async->event_head + 1) % REPL_UI_EVENT_MAX;
  async->event_count--;
  async->event_bytes -= event->len;
  pthread_cond_signal(&async->space);
  pthread_mutex_unlock(&async->lock);
  return 1;
}

static void repl_async_delta(const char *text, void *ud) {
  repl_async_text(ud, REPL_UI_DELTA, text);
}

static void repl_async_final(const char *text, void *ud) {
  repl_async_text(ud, REPL_UI_FINAL, text);
}

static void repl_async_diagnostic(const char *text, void *ud) {
  repl_async_text(ud, REPL_UI_DIAGNOSTIC, text);
}

static void repl_async_reasoning(const char *text, void *ud) {
  ReplAsync *async = ud;
  if (!motiris_thinking_visible(async->agent)) return;
  repl_async_text(async, REPL_UI_REASONING, text);
}

static void repl_async_tool(const char *name, const char *args,
                            const char *result, void *ud) {
  char abuf[80], rbuf[96], line[512];
  clip(abuf, sizeof abuf, args ? args : "{}", 60);
  clip(rbuf, sizeof rbuf, result ? result : "", 72);
  int is_err = result && strstr(result, "\"error\"");
  if (is_err)
    snprintf(line, sizeof line,
             "  " C_YELLOW "⚙ %s" C_RESET "(%s) " C_RED "✗ %s" C_RESET,
             name, abuf, rbuf);
  else
    snprintf(line, sizeof line,
             "  " C_YELLOW "⚙ %s" C_RESET "(%s) " C_DIM "→ %s" C_RESET,
             name, abuf, rbuf);
  repl_async_text(ud, REPL_UI_TOOL, line);
}

static char *repl_bounded_copy(const char *text) {
  if (!text) text = "";
  size_t len = strnlen(text, REPL_UI_CHUNK);
  char *copy = malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, text, len);
  copy[len] = '\0';
  return copy;
}

static MotirisApprovalDecision repl_async_approval(const char *name,
                                                    const char *args,
                                                    void *ud) {
  ReplAsync *async = ud;
  char *name_copy = repl_bounded_copy(name);
  char *args_copy = repl_bounded_copy(args);
  if (!name_copy || !args_copy) {
    free(name_copy);
    free(args_copy);
    return MOTIRIS_APPROVAL_DENY;
  }

  pthread_mutex_lock(&async->lock);
  while (async->approval_pending && !async->closing)
    pthread_cond_wait(&async->approval_condition, &async->lock);
  if (async->closing) {
    pthread_mutex_unlock(&async->lock);
    free(name_copy);
    free(args_copy);
    return MOTIRIS_APPROVAL_DENY;
  }
  async->approval_name = name_copy;
  async->approval_args = args_copy;
  async->approval_decision = MOTIRIS_APPROVAL_DENY;
  async->approval_done = 0;
  async->approval_pending = 1;
  pthread_mutex_unlock(&async->lock);
  repl_async_wake(async);

  pthread_mutex_lock(&async->lock);
  while (!async->approval_done && !async->closing)
    pthread_cond_wait(&async->approval_condition, &async->lock);
  MotirisApprovalDecision decision = async->closing
      ? MOTIRIS_APPROVAL_DENY : async->approval_decision;
  free(async->approval_name);
  free(async->approval_args);
  async->approval_name = async->approval_args = NULL;
  async->approval_pending = 0;
  async->approval_done = 0;
  pthread_mutex_unlock(&async->lock);
  return decision;
}

static int repl_async_cancelled(void *ud) {
  (void)ud;
  return atomic_load_explicit(&generation_interrupted, memory_order_relaxed);
}

static void *repl_async_worker(void *ud) {
  ReplAsync *async = ud;
  motiris_set_cancel_cb(async->agent, repl_async_cancelled, async);
  int rc = motiris_run(async->agent);
  motiris_set_cancel_cb(async->agent, NULL, NULL);
  pthread_mutex_lock(&async->lock);
  async->run_rc = rc;
  async->finished = 1;
  pthread_mutex_unlock(&async->lock);
  repl_async_wake(async);
  return NULL;
}

static int repl_async_approval_pending(ReplAsync *async) {
  pthread_mutex_lock(&async->lock);
  int pending = async->approval_pending;
  pthread_mutex_unlock(&async->lock);
  return pending;
}

static int repl_async_collect_ready(ReplAsync *async) {
  pthread_mutex_lock(&async->lock);
  int ready = async->finished && async->event_count == 0;
  pthread_mutex_unlock(&async->lock);
  return ready;
}

static void repl_async_prepare_run(ReplAsync *async) {
  pthread_mutex_lock(&async->lock);
  async->finished = 0;
  async->run_rc = 0;
  pthread_mutex_unlock(&async->lock);
}

static void repl_async_request_shutdown(ReplAsync *async) {
  pthread_mutex_lock(&async->lock);
  async->closing = 1;
  async->approval_decision = MOTIRIS_APPROVAL_DENY;
  async->approval_done = 1;
  async->approval_pending = 0;
  pthread_cond_broadcast(&async->approval_condition);
  pthread_cond_broadcast(&async->space);
  pthread_mutex_unlock(&async->lock);
  repl_async_wake(async);
}

static int repl_fifo_push(ReplFifo *fifo, char *line) {
  size_t len = strlen(line);
  if (fifo->count == REPL_FIFO_MAX || len > REPL_FIFO_BYTES - fifo->bytes)
    return -1;
  size_t slot = (fifo->head + fifo->count) % REPL_FIFO_MAX;
  fifo->lines[slot] = line;
  fifo->lengths[slot] = len;
  fifo->count++;
  fifo->bytes += len;
  return 0;
}

static char *repl_fifo_pop(ReplFifo *fifo) {
  if (!fifo->count) return NULL;
  char *line = fifo->lines[fifo->head];
  fifo->bytes -= fifo->lengths[fifo->head];
  fifo->lines[fifo->head] = NULL;
  fifo->lengths[fifo->head] = 0;
  fifo->head = (fifo->head + 1) % REPL_FIFO_MAX;
  fifo->count--;
  return line;
}

static void repl_fifo_clear(ReplFifo *fifo) {
  char *line;
  while ((line = repl_fifo_pop(fifo)) != NULL) free(line);
}

static void repl_tty_output_mode(int cooked) {
  struct termios terminal;
  if (tcgetattr(STDIN_FILENO, &terminal) != 0) return;
  if (cooked) terminal.c_oflag |= OPOST;
  else terminal.c_oflag &= (tcflag_t)~OPOST;
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}

static void repl_tty_hide(ReplTTY *tty) {
  if (tty->hidden) return;
  fflush(stdout);
  linenoiseHide(&tty->editor);
  repl_tty_output_mode(1);
  tty->hidden = 1;
}

static void repl_tty_show(ReplTTY *tty) {
  if (!tty->hidden) return;
  fflush(stdout);
  repl_tty_output_mode(0);
  tty->hidden = 0;
  tty->output_needs_newline = 0;
  linenoiseShow(&tty->editor);
  /* Keep an empty idle prompt byte-compatible with linenoiseEditStart(). */
  if (!tty->editor.len && !tty->editor.menu_rows &&
      !tty->editor.in_completion) {
    static const char prompt[] = "\rmotiris> ";
    if (write(STDOUT_FILENO, prompt, sizeof prompt - 1) < 0) {}
  }
}

static void repl_tty_render_event(ReplTTY *tty, const ReplUiEvent *event) {
  ReplState *state = tty->state;
  if (event->type == REPL_UI_REASONING) {
    if (!state->reasoning_active) {
      printf(C_DIM "\n[thinking]\n" C_RESET);
      state->reasoning_active = 1;
    }
    printf(C_DIM);
    fwrite(event->text, 1, event->len, stdout);
    printf(C_RESET);
  } else if (event->type == REPL_UI_TOOL ||
             event->type == REPL_UI_DIAGNOSTIC) {
    if (tty->output_needs_newline) {
      printf("\n");
      tty->output_needs_newline = 0;
    }
    fwrite(event->text, 1, event->len,
           event->type == REPL_UI_DIAGNOSTIC ? stderr : stdout);
    if (event->type == REPL_UI_TOOL) printf("\n");
  } else {
    if (state->reasoning_active) {
      printf(C_RESET "\n");
      state->reasoning_active = 0;
    }
    fwrite(event->text, 1, event->len, stdout);
  }
  tty->output_needs_newline = event->type != REPL_UI_TOOL &&
                              event->len && event->text[event->len - 1] != '\n';
  fflush(stderr);
  fflush(stdout);
}

static void repl_tty_drain_events(ReplTTY *tty) {
  ReplUiEvent event;
  int rendered = 0;
  while (repl_async_pop(&tty->async, &event)) {
    if (!rendered) {
      repl_tty_hide(tty);
      rendered = 1;
    }
    repl_tty_render_event(tty, &event);
    free(event.text);
  }
  /* Do not overwrite an unfinished answer with the prompt. The next key
   * restores the editor on a fresh line; turn completion does the same. */
  if (rendered && !tty->worker_active) repl_tty_show(tty);
}

static void repl_tty_answer_approval(ReplTTY *tty) {
  repl_tty_drain_events(tty);
  repl_tty_hide(tty);
  MotirisApprovalDecision decision = approval_card(tty->async.approval_name,
                                                    tty->async.approval_args,
                                                    tty->state);
  pthread_mutex_lock(&tty->async.lock);
  if (tty->async.approval_pending) {
    tty->async.approval_decision = decision;
    tty->async.approval_done = 1;
    tty->async.approval_pending = 0;
    pthread_cond_signal(&tty->async.approval_condition);
  }
  pthread_mutex_unlock(&tty->async.lock);
  repl_tty_show(tty);
}

static void repl_tty_editor_next(ReplTTY *tty, const char *restore) {
  linenoiseEditNext(&tty->editor);
  if (restore) {
    size_t len = strlen(restore);
    if (len <= tty->editor.buflen) {
      memcpy(tty->editor.buf, restore, len + 1);
      tty->editor.len = tty->editor.pos = len;
    }
  }
}

static void repl_tty_accept(ReplTTY *tty, char *line) {
  repl_tty_hide(tty);
  if (!*line) {
    free(line);
    repl_tty_editor_next(tty, NULL);
    repl_tty_show(tty);
    return;
  }

  if (repl_fifo_push(&tty->fifo, line) != 0) {
    printf(C_YELLOW "input queue full; line kept in editor" C_RESET "\n");
    repl_tty_editor_next(tty, line);
    free(line);
    repl_tty_show(tty);
    return;
  }

  printf("%s\n", line);
  linenoiseHistoryAdd(line);
  tty->history_dirty = 1;
  if (tty->worker_active)
    printf(C_DIM "[queued %zu]" C_RESET "\n", tty->fifo.count);
  if (!tty->worker_active && tty->history_dirty) {
    linenoiseHistorySave(tty->hist);
    tty->history_dirty = 0;
  }
  repl_tty_editor_next(tty, NULL);
  repl_tty_show(tty);
}

static void repl_tty_finish_turn(ReplTTY *tty, int rc) {
  MotirisAgent *a = tty->agent;
  ReplState *state = tty->state;
  struct timespec finished;
  clock_gettime(CLOCK_MONOTONIC, &finished);
  long ms = (finished.tv_sec - tty->started.tv_sec) * 1000L
          + (finished.tv_nsec - tty->started.tv_nsec) / 1000000L;
  long di = motiris_tokens_in(a) - tty->tokens_in;
  long dout = motiris_tokens_out(a) - tty->tokens_out;
  if (state->reasoning_active) {
    printf(C_RESET "\n");
    state->reasoning_active = 0;
  }
  if (tty->color) printf(C_RESET "\n");
  if (rc == MOTIRIS_RUN_CANCELLED)
    fprintf(stderr, C_YELLOW "generation cancelled; enter a new instruction"
            C_RESET "\n");
  else if (rc) fprintf(stderr, C_RED "motiris: %s" C_RESET "\n",
                       motiris_last_error(a));
#ifdef MOTIRIS_TTS_TOOLS
  if (!rc) tts_new_reply(a);
#endif
#ifndef MOTIRIS_NO_PERSISTENCE
  {
    const char *error = NULL;
    if (motiris_session_auto_save(a, state->session_id, "repl", NULL, &error))
      fprintf(stderr, C_YELLOW "automatic session not saved: %s" C_RESET "\n",
              error ? error : "snapshot failed");
  }
#endif
  if (sess_id && !rc) {
    const char *error = NULL;
    if (motiris_session_checkpoint("sessions", sess_id, a, sess_id, &error))
      fprintf(stderr, C_YELLOW "session checkpoint not saved: %s" C_RESET "\n",
              error ? error : "snapshot failed");
  }
  if (di || dout)
    printf(C_DIM "[tokens " C_RESET C_CYAN "↑%ld" C_RESET C_DIM
           " " C_RESET C_GREEN "↓%ld" C_RESET C_DIM
           " · total %ld · %ldms · %s]" C_RESET "\n",
           di, dout, motiris_tokens_in(a) + motiris_tokens_out(a),
           ms, motiris_model(a));
  printf("\n");
}

static void repl_tty_dispatch(ReplTTY *tty) {
  char *line = repl_fifo_pop(&tty->fifo);
  if (!line) return;
  repl_tty_hide(tty);
  int cmd = handle_command(tty->agent, tty->state, line);
  if (cmd == 1) {
    free(line);
    tty->exiting = 1;
    repl_tty_show(tty);
    return;
  }
  if (cmd == 0) {
    free(line);
    repl_tty_show(tty);
    return;
  }

  motiris_add_user(tty->agent, line);
  free(line);
  printf(C_DIM "→ you" C_RESET "\n");
  if (tty->color) printf(C_CYAN);
  tty->state->reasoning_active = 0;
  tty->tokens_in = motiris_tokens_in(tty->agent);
  tty->tokens_out = motiris_tokens_out(tty->agent);
  clock_gettime(CLOCK_MONOTONIC, &tty->started);
  atomic_store_explicit(&generation_interrupted, 0, memory_order_relaxed);
  repl_async_prepare_run(&tty->async);
  int rc = pthread_create(&tty->worker, NULL, repl_async_worker, &tty->async);
  if (rc != 0) {
    if (tty->color) printf(C_RESET "\n");
    fprintf(stderr, C_RED "motiris: cannot start generation: %s" C_RESET "\n",
            strerror(rc));
#ifndef MOTIRIS_NO_PERSISTENCE
    /* The user message is already canonical; preserve it even though no
     * assistant turn could be started. Later queued input remains FIFO. */
    const char *error = NULL;
    if (motiris_session_auto_save(tty->agent, tty->state->session_id,
                                  "repl", NULL, &error))
      fprintf(stderr, C_YELLOW "automatic session not saved: %s" C_RESET "\n",
              error ? error : "snapshot failed");
#endif
    printf("\n");
    repl_tty_show(tty);
    return;
  }
  tty->worker_active = 1;
  repl_tty_show(tty);
}

static void repl_tty_save_history(ReplTTY *tty) {
  if (!tty->history_dirty || tty->worker_active) return;
  linenoiseHistorySave(tty->hist);
  tty->history_dirty = 0;
}

static void repl_tty_collect(ReplTTY *tty) {
  if (!tty->worker_active) return;
  pthread_join(tty->worker, NULL);
  tty->worker_active = 0;
  repl_tty_save_history(tty);
  repl_tty_hide(tty);
  repl_tty_finish_turn(tty, tty->async.run_rc);
  repl_tty_show(tty);
}

static void repl_tty_drain_pipe(ReplTTY *tty) {
  unsigned char buf[64];
  for (;;) {
    ssize_t n = read(tty->async.pipefd[0], buf, sizeof buf);
    if (n > 0) continue;
    if (n < 0 && errno == EINTR) continue;
    break;
  }
}

static int repl_tty_input_hup(void) {
  struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
  int ready;
  do {
    ready = poll(&input, 1, 0);
  } while (ready < 0 && errno == EINTR);
  return ready > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL));
}

static int repl_tty_supported(void) {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
  const char *term = getenv("TERM");
  return !term || (strcasecmp(term, "dumb") && strcasecmp(term, "cons25") &&
                   strcasecmp(term, "emacs"));
}

static int repl_tty_loop(MotirisAgent *a, ReplState *state,
                         const char *hist, int color) {
  ReplTTY tty;
  memset(&tty, 0, sizeof tty);
  tty.agent = a;
  tty.state = state;
  tty.hist = hist;
  tty.color = color;
  if (repl_async_init(&tty.async, a) != 0) return -1;

  char *edit_buf = malloc(REPL_EDIT_INITIAL);
  if (!edit_buf) {
    repl_async_destroy(&tty.async);
    return -1;
  }
  if (linenoiseEditStart(&tty.editor, STDIN_FILENO, STDOUT_FILENO,
                         edit_buf, REPL_EDIT_INITIAL, "motiris> ") != 0) {
    linenoiseEditStop(&tty.editor);
    free(tty.editor.buf);
    repl_async_destroy(&tty.async);
    return -1;
  }
  tty.editor.buflen_max = REPL_FIFO_BYTES;
  if (linenoiseEditSetNonblocking(&tty.editor, 1) != 0) {
    linenoiseEditStop(&tty.editor);
    free(tty.editor.buf);
    repl_async_destroy(&tty.async);
    return -1;
  }

  struct sigaction action = {0};
  action.sa_handler = generation_sigint;
  sigemptyset(&action.sa_mask);
  atomic_store_explicit(&generation_interrupted, 0, memory_order_relaxed);
  if (sigaction(SIGINT, &action, &tty.previous_sigint) == 0)
    tty.sigint_installed = 1;
  action.sa_handler = terminal_sighup;
  atomic_store_explicit(&terminal_hangup, 0, memory_order_relaxed);
  if (sigaction(SIGHUP, &action, &tty.previous_sighup) == 0)
    tty.sighup_installed = 1;

  motiris_set_stream_cb(a, repl_async_delta, &tty.async);
  motiris_set_reasoning_cb(a, repl_async_reasoning, &tty.async);
  motiris_set_final_cb(a, repl_async_final, &tty.async);
  motiris_set_diagnostic_cb(a, repl_async_diagnostic, &tty.async);
  motiris_set_print_answer(a, 0);
  motiris_set_approval_callback(a, repl_async_approval, &tty.async);
  motiris_set_tool_hook(a, repl_async_tool, &tty.async);

  while (!tty.exiting) {
    /* Do not dispatch more queued work after a terminal hangup, including
     * when an approval/config card was the reader that observed the EOF. */
    if (atomic_load_explicit(&terminal_hangup, memory_order_relaxed) ||
        repl_tty_input_hup()) {
      tty.exiting = 1;
      continue;
    }
    if (repl_async_approval_pending(&tty.async)) {
      repl_tty_answer_approval(&tty);
      continue;
    }
    if (tty.worker_active && repl_async_collect_ready(&tty.async)) {
      repl_tty_collect(&tty);
      continue;
    }
    if (!tty.worker_active && tty.fifo.count) {
      repl_tty_dispatch(&tty);
      continue;
    }

    struct pollfd fds[2] = {
      { .fd = STDIN_FILENO, .events = POLLIN },
      { .fd = tty.async.pipefd[0], .events = POLLIN }
    };
    int ready = poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      repl_tty_hide(&tty);
      fprintf(stderr, C_RED "motiris: terminal poll: %s" C_RESET "\n",
              strerror(errno));
      repl_tty_show(&tty);
      tty.exiting = 1;
      continue;
    }
    if (fds[1].revents & POLLIN) {
      repl_tty_drain_pipe(&tty);
      repl_tty_drain_events(&tty);
    }
    if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      tty.exiting = 1;
      continue;
    }
    if (fds[0].revents & POLLIN) {
      if (tty.hidden) {
        if (tty.output_needs_newline) printf("\n");
        repl_tty_show(&tty);
      }
      errno = 0;
      char *line = linenoiseEditFeed(&tty.editor);
      if (line == linenoiseEditMore) {
        /* Keep editing the same draft. */
      } else if (line) {
        repl_tty_accept(&tty, line);
      } else if (errno == EAGAIN || errno == ECANCELED) {
        if (tty.worker_active) {
          atomic_store_explicit(&generation_interrupted, 1, memory_order_relaxed);
        } else if (errno == ECANCELED && tty.editor.in_completion) {
          /* Nonblocking Escape bypasses completeLine so it can cancel a
           * worker. At idle it must still dismiss the highlighted completion
           * without applying it to the next key. */
          tty.editor.in_completion = 0;
          repl_tty_hide(&tty);
          repl_tty_show(&tty);
        }
      } else {
        int error = errno;
        if (error && error != ENOENT && error != EIO) {
          repl_tty_hide(&tty);
          fprintf(stderr, C_RED "motiris: terminal input: %s" C_RESET "\n",
                  strerror(error));
          repl_tty_show(&tty);
        }
        tty.exiting = 1;
        continue;
      }
    }
    if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
      tty.exiting = 1;
  }

  atomic_store_explicit(&generation_interrupted, 1, memory_order_relaxed);
  repl_async_request_shutdown(&tty.async);
  if (tty.worker_active) {
    pthread_join(tty.worker, NULL);
    /* Drain while worker_active still suppresses prompt redraw. The normal
     * finish path below establishes the fresh-line boundary exactly once. */
    repl_tty_drain_events(&tty);
    tty.worker_active = 0;
    repl_tty_save_history(&tty);
    repl_tty_hide(&tty);
    repl_tty_finish_turn(&tty, tty.async.run_rc);
    repl_tty_show(&tty);
  }
  repl_tty_save_history(&tty);
  repl_fifo_clear(&tty.fifo);
  repl_tty_show(&tty);
  linenoiseEditStop(&tty.editor);
  free(tty.editor.buf);
  if (tty.sigint_installed) sigaction(SIGINT, &tty.previous_sigint, NULL);
  if (tty.sighup_installed) sigaction(SIGHUP, &tty.previous_sighup, NULL);
  repl_async_destroy(&tty.async);
  return 0;
}

int motiris_repl(MotirisAgent *a) {
  ReplState state = {
    .agent = a,
    .reasoning_active = 0,
    .session_id = motiris_session_auto_id(),
  };
  motiris_set_stream_cb(a, delta_print, &state);
  motiris_set_reasoning_cb(a, reasoning_print, &state);
  if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
    motiris_set_approval_callback(a, approval_card, &state);
  motiris_set_tool_hook(a, repl_tool_hook, NULL);
  int color = isatty(1) ? 1 : 0;

  linenoiseSetCompletionCallback(repl_completion);
  linenoiseSetHintsCallback(repl_hints);
  linenoiseSetMenuCallback(repl_menu);
  linenoiseSetMenuSelectCallback(repl_menu_select);

  char hist[1024];
  expand_home(hist, sizeof hist, hist_path());
  linenoiseHistoryLoad(hist);
  struct stat hst;
  int first_run = stat(hist, &hst) || hst.st_size == 0;

#ifndef MOTIRIS_NO_PERSISTENCE
  /* interactive sessions log to ~/.local/share/motiris/sessions/ */
  if (isatty(0)) {
    char id[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(id, sizeof id, "repl-%s-%ld", ts, (long)getpid());
    sess_id = strdup(id);
  }
#endif

  print_banner(a);
  if (first_run)
    printf(C_DIM "  try: /help (commands) · /sessions (review past) · "
           "/skills (list skills)\n" C_RESET);

  if (!repl_tty_supported() || repl_tty_loop(a, &state, hist, color) != 0)
    repl_sync_loop(a, &state, hist, color);
  printf(C_DIM "bye" C_RESET "\n");
  free(sess_id);
  sess_id = NULL;
  free(state.session_id);
  return 0;
}
