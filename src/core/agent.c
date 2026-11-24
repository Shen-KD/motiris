/* agent.c - the motiris agent loop.
 *
 *   assemble request -> transport -> parse -> tool_calls?
 *        ^                                          |
 *        |                   append tool results  <-+ (execute tools)
 *        +------------ final answer printed, done  (finish_reason == stop)
 *
 * Messages are kept as a cJSON array; each round appends. A positive
 * max_steps bounds the loop; zero deliberately leaves it unlimited.
 */
#include "motiris.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 128
#define HISTORY_MAX_MESSAGES 256
/* Host tools can return bounded, escaped multi-MiB results. The live cap is
 * an allocation ceiling, not a preallocation; MCU embedders can set it lower. */
#define HISTORY_MAX_BYTES (16 * 1024 * 1024)
#define DEF_BASE_URL "https://api.openai.com/v1/chat/completions"
#define AUTH_HEADER_SIZE 512
#define AUTH_HEADER_PREFIX "Authorization: Bearer "
#define AUTH_MAX_KEY_BYTES (AUTH_HEADER_SIZE - sizeof(AUTH_HEADER_PREFIX))

struct MotirisAgent {
  char *model;
  char *base_url;
  char *api_key;
  char *system;
  cJSON *messages;         /* array of OpenAI message objects */
  MotirisTool tools[MAX_TOOLS];
  int ntools;
  char transport[16];
  int max_steps;
  int verbose;
  int print_answer;
  int stream;
  void (*on_delta)(const char *, void *);
  void *delta_ud;
  void (*on_final)(const char *, void *);
  void *final_ud;
  void (*on_diagnostic)(const char *, void *);
  void *diagnostic_ud;
  void (*on_reasoning)(const char *, void *);
  void *reasoning_ud;
  char reasoning_effort[8];
  int thinking_visible;
  MotirisCancelCallback *cancel;
  void *cancel_ud;
  int tools_on, plugins_on;
  int goal_check;
  int approval_required;
  int workspace_guard;
  int compact_enabled;
  size_t compact_threshold, compact_keep, compact_max_chars;
  MotirisCompactionCallback *compactor;
  void *compact_ud;
  MotirisApprovalCallback *approval;
  void *approval_ud;
  unsigned char approval_grants[(MAX_TOOLS + 7) / 8];
  char *plugin_dir;
  /* tool-call observers (multi-slot; plugins and repl can both watch) */
  struct { MotirisToolHook *cb; void *ud; } tool_hooks[8];
  int ntool_hooks;
  long tok_in, tok_out;
  MotirisProvider providers[8];
  int nproviders;
  char *last_error;
  /* Per-agent live history and provider replay budgets. */
  size_t history_max_messages, history_max_bytes;
  size_t replay_max_messages, replay_max_bytes;
  cJSON *run_start;       /* latest user turn; never evicted during its run */
  int history_error;
};

static void agent_diagnostic(MotirisAgent *a, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  if (!a->on_diagnostic) {
    vfprintf(stderr, format, ap);
    va_end(ap);
    return;
  }
  va_list measure;
  va_copy(measure, ap);
  int length = vsnprintf(NULL, 0, format, measure);
  va_end(measure);
  if (length >= 0) {
    char *message = malloc((size_t)length + 1);
    if (message) {
      vsnprintf(message, (size_t)length + 1, format, ap);
      a->on_diagnostic(message, a->diagnostic_ud);
      free(message);
    }
  }
  va_end(ap);
}

/* transport.c */
char *motiris_transport_send(const char *backend, const char *url,
                             const char *auth, const char *body,
                             MotirisCancelCallback *cancel, void *cancel_ud,
                             char **err);
char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud,
                                    void (*on_reasoning)(const char *, void *),
                                    void *reasoning_ud,
                                    MotirisCancelCallback *cancel,
                                    void *cancel_ud, char **err);

/* tools.c */
int motiris_register_core_tools_count(void);
const MotirisTool *motiris_core_tools(void);

void motiris_register_core_tools(MotirisAgent *a) {
  if (!a->tools_on) return;
  for (int i = 0; i < motiris_register_core_tools_count(); i++)
    motiris_register_tool(a, &motiris_core_tools()[i]);
#ifdef MOTIRIS_VISION_TOOLS
  motiris_register_vision_tools(a);
#endif
}

static char *sdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (copy) memcpy(copy, s, n);
  return copy;
}

void motiris_set_plugin_dir(MotirisAgent *a, const char *d) {
  free(a->plugin_dir);
  a->plugin_dir = sdup(d);
}
void motiris_set_tools_enabled(MotirisAgent *a, int on) { a->tools_on = !!on; }
int motiris_tools_enabled(MotirisAgent *a) { return a->tools_on; }
void motiris_set_plugins_enabled(MotirisAgent *a, int on) { a->plugins_on = !!on; }
int motiris_plugins_enabled(MotirisAgent *a) { return a->plugins_on; }
const char *motiris_plugin_dir(MotirisAgent *a) { return a->plugin_dir; }

void motiris_add_provider(MotirisAgent *a, const MotirisProvider *p) {
  if (!p || !p->model || !p->base_url || a->nproviders >= 8) return;
  MotirisProvider *d = &a->providers[a->nproviders++];
  d->name = sdup(p->name);
  d->model = sdup(p->model);
  d->base_url = sdup(p->base_url);
  d->api_key = sdup(p->api_key);
}
int motiris_provider_count(MotirisAgent *a) { return a->nproviders; }

static const char *pname(const MotirisAgent *a, int pi) {
  return a->providers[pi].name ? a->providers[pi].name : a->providers[pi].model;
}

static size_t env_limit(const char *name, size_t dflt, size_t maximum) {
  const char *s = getenv(name);
  if (!s || !*s) return dflt;
  char *end = NULL;
  unsigned long value = strtoul(s, &end, 10);
  if (!end || *end || end == s || !value || value > maximum) {
    fprintf(stderr, "motiris: invalid %s=%s; using %zu\n", name, s, dflt);
    return dflt;
  }
  return (size_t)value;
}

MotirisAgent *motiris_new(void) {
  MotirisAgent *a = calloc(1, sizeof *a);
  const char *u = getenv("MOTIRIS_BASE_URL");
  a->base_url = sdup(u && *u ? u : DEF_BASE_URL);
  const char *m = getenv("MOTIRIS_MODEL");
  a->model = sdup(m && *m ? m : "gpt-4o-mini");
  a->messages = cJSON_CreateArray();
  strcpy(a->transport, "auto");
  a->max_steps = 0;
  a->compact_threshold = 32;
  a->compact_keep = 8;
  a->compact_max_chars = 8192;
  a->print_answer = 1;
  a->stream = 1;
  a->history_max_messages = env_limit("MOTIRIS_HISTORY_MAX_MESSAGES",
                                      HISTORY_MAX_MESSAGES, HISTORY_MAX_MESSAGES);
  a->history_max_bytes = env_limit("MOTIRIS_HISTORY_MAX_BYTES",
                                   HISTORY_MAX_BYTES, HISTORY_MAX_BYTES);
  a->replay_max_messages = env_limit("MOTIRIS_REPLAY_MAX_MESSAGES",
                                     HISTORY_MAX_MESSAGES, HISTORY_MAX_MESSAGES);
  a->replay_max_bytes = env_limit("MOTIRIS_REPLAY_MAX_BYTES",
                                  HISTORY_MAX_BYTES, HISTORY_MAX_BYTES);
  /* Validate on the creating thread. transport.c applies the same setting
   * silently per request so worker threads never write configuration warnings
   * directly over an interactive prompt. */
  (void)env_limit("MOTIRIS_RESPONSE_MAX_BYTES",
                  HISTORY_MAX_BYTES, HISTORY_MAX_BYTES);
  a->tools_on = 1;
  a->plugins_on = 1;
  const char *gc = getenv("MOTIRIS_GOAL_CHECK");
  if (gc && gc[0] && gc[0] != '0') a->goal_check = 1;
  return a;
}

void motiris_free(MotirisAgent *a) {
  if (!a) return;
  free(a->model);
  free(a->base_url);
  free(a->api_key);
  free(a->system);
  free(a->plugin_dir);
  for (int i = 0; i < a->nproviders; i++) {
    free((char *)a->providers[i].name);
    free((char *)a->providers[i].model);
    free((char *)a->providers[i].base_url);
    free((char *)a->providers[i].api_key);
  }
  free(a->last_error);
  cJSON_Delete(a->messages);
  free(a);
}

void motiris_set_model(MotirisAgent *a, const char *m) {
  free(a->model); a->model = sdup(m);
}
const char *motiris_model(MotirisAgent *a) { return a->model; }
int motiris_tool_count(MotirisAgent *a) { return a->ntools; }
const char *motiris_tool_name(MotirisAgent *a, int i) {
  return (i >= 0 && i < a->ntools) ? a->tools[i].name : NULL;
}
void motiris_set_base_url(MotirisAgent *a, const char *u) {
  free(a->base_url); a->base_url = sdup(u);
}
void motiris_set_api_key(MotirisAgent *a, const char *k) {
  free(a->api_key); a->api_key = sdup(k);
}
void motiris_set_transport(MotirisAgent *a, const char *n) {
  snprintf(a->transport, sizeof a->transport, "%s", n ? n : "auto");
}
#ifdef MOTIRIS_VISION_TOOLS
const char *motiris_base_url(MotirisAgent *a) { return a->base_url; }
const char *motiris_api_key(MotirisAgent *a) { return a->api_key; }
const char *motiris_transport_name(MotirisAgent *a) { return a->transport; }
#endif
void motiris_set_stream(MotirisAgent *a, int on) { a->stream = !!on; }
int motiris_stream_enabled(MotirisAgent *a) { return a->stream; }
void motiris_set_stream_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                           void *ud) { a->on_delta = cb; a->delta_ud = ud; }
void motiris_set_final_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                          void *ud) { a->on_final = cb; a->final_ud = ud; }
void motiris_set_diagnostic_cb(MotirisAgent *a,
                               void (*cb)(const char *, void *), void *ud) {
  a->on_diagnostic = cb;
  a->diagnostic_ud = ud;
}
void motiris_set_reasoning_cb(MotirisAgent *a,
                              void (*cb)(const char *, void *), void *ud) {
  a->on_reasoning = cb;
  a->reasoning_ud = ud;
}
void motiris_set_thinking_visible(MotirisAgent *a, int on) {
  a->thinking_visible = !!on;
}
int motiris_thinking_visible(MotirisAgent *a) { return a->thinking_visible; }
void motiris_set_cancel_cb(MotirisAgent *a, MotirisCancelCallback *cb, void *ud) {
  a->cancel = cb;
  a->cancel_ud = ud;
}
static int run_cancelled(const MotirisAgent *a) {
  return a->cancel && a->cancel(a->cancel_ud);
}

typedef struct {
  void (*cb)(const char *, void *);
  void *ud;
  int seen;
} DeltaForward;

/* Remember only deltas actually delivered by the streaming backend. Buffered
 * fallback leaves seen=0, so the aggregated final answer is still printed. */
static void forward_delta(const char *text, void *ud) {
  DeltaForward *forward = ud;
  if (text && *text) forward->seen = 1;
  forward->cb(text, forward->ud);
}

typedef struct {
  void (*cb)(const char *, void *);
  void *ud;
  int seen;
} ReasoningForward;

static void forward_reasoning(const char *text, void *ud) {
  ReasoningForward *forward = ud;
  if (text && *text) forward->seen = 1;
  forward->cb(text, forward->ud);
}
void motiris_set_tool_hook(MotirisAgent *a, MotirisToolHook cb, void *ud) {
  a->ntool_hooks = 0; /* replace any existing observers */
  motiris_add_tool_hook(a, cb, ud);
}
void motiris_add_tool_hook(MotirisAgent *a, MotirisToolHook cb, void *ud) {
  if (!a || !cb || a->ntool_hooks >= 8) return;
  a->tool_hooks[a->ntool_hooks].cb = cb;
  a->tool_hooks[a->ntool_hooks].ud = ud;
  a->ntool_hooks++;
}
long motiris_tokens_in(MotirisAgent *a)  { return a->tok_in; }
long motiris_tokens_out(MotirisAgent *a) { return a->tok_out; }
void motiris_set_max_steps(MotirisAgent *a, int n) {
  a->max_steps = n > 0 ? n : 0;
}
int motiris_max_steps(MotirisAgent *a) { return a->max_steps; }
void motiris_set_verbose(MotirisAgent *a, int on) { a->verbose = on; }
void motiris_set_print_answer(MotirisAgent *a, int on) { a->print_answer = !!on; }
void motiris_set_goal_check(MotirisAgent *a, int on) { a->goal_check = !!on; }
void motiris_set_workspace_guard(MotirisAgent *a, int on) {
  a->workspace_guard = !!on;
}
int motiris_workspace_guard_enabled(const MotirisAgent *a) {
  return a->workspace_guard;
}
void motiris_set_approval_required(MotirisAgent *a, int on) {
  a->approval_required = !!on;
}
int motiris_approval_required(MotirisAgent *a) { return a->approval_required; }
void motiris_set_approval_callback(MotirisAgent *a,
                                   MotirisApprovalCallback *cb, void *ud) {
  a->approval = cb;
  a->approval_ud = ud;
}
void motiris_clear_approval_grants(MotirisAgent *a) {
  memset(a->approval_grants, 0, sizeof a->approval_grants);
}
static void set_error(MotirisAgent *a, const char *msg, const char *arg);

void motiris_set_compactor(MotirisAgent *a, MotirisCompactionCallback *cb,
                           void *ud) {
  a->compactor = cb;
  a->compact_ud = ud;
}
int motiris_set_compaction(MotirisAgent *a, int enabled, size_t threshold,
                           size_t keep, size_t max_chars) {
  if (threshold < 4 || threshold > 256 || !keep || keep >= threshold ||
      max_chars < 256 || max_chars > 65536) {
    set_error(a, "%s", "invalid context compaction limits");
    return -1;
  }
  a->compact_enabled = !!enabled;
  a->compact_threshold = threshold;
  a->compact_keep = keep;
  a->compact_max_chars = max_chars;
  return 0;
}

const char *motiris_last_error(MotirisAgent *a) {
  static const char none[] = "";
  return a->last_error ? a->last_error : none;
}

static void set_error(MotirisAgent *a, const char *msg, const char *arg) {
  free(a->last_error);
  size_t n = strlen(msg) + (arg ? strlen(arg) : 0) + 2;
  a->last_error = malloc(n);
  snprintf(a->last_error, n, msg, arg ? arg : "");
}

void motiris_set_system(MotirisAgent *a, const char *sys) {
  free(a->system);
  a->system = sdup(sys);
}

void motiris_append_system(MotirisAgent *a, const char *text) {
  if (!text || !*text) return;
  size_t old = a->system ? strlen(a->system) : 0;
  size_t add = strlen(text);
  char *joined = malloc(old + add + 3);
  if (!joined) {
    set_error(a, "%s", "out of memory while loading system context");
    return;
  }
  if (old) {
    memcpy(joined, a->system, old);
    memcpy(joined + old, "\n\n", 2);
  }
  memcpy(joined + old + (old ? 2 : 0), text, add + 1);
  free(a->system);
  a->system = joined;
}

int motiris_set_reasoning_effort(MotirisAgent *a, const char *effort) {
  if (effort && *effort && strcmp(effort, "off") &&
      strcmp(effort, "low") && strcmp(effort, "medium") &&
      strcmp(effort, "high")) {
    set_error(a, "%s", "reasoning effort must be off, low, medium or high");
    return -1;
  }
  if (!effort || !*effort || !strcmp(effort, "off")) a->reasoning_effort[0] = '\0';
  else snprintf(a->reasoning_effort, sizeof a->reasoning_effort, "%s", effort);
  return 0;
}
const char *motiris_reasoning_effort(MotirisAgent *a) {
  return a->reasoning_effort;
}

static int history_fail(MotirisAgent *a, const char *message) {
  a->history_error = 1;
  set_error(a, "%s", message);
  return -1;
}

static int message_size(const cJSON *message, size_t *size) {
  char *printed = cJSON_PrintUnformatted(message);
  if (!printed) return -1;
  *size = strlen(printed);
  free(printed);
  return 0;
}

static const char *message_role(const cJSON *message) {
  const char *role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "role"));
  return role ? role : "";
}

static int history_measure(MotirisAgent *a, size_t *count, size_t *bytes) {
  *count = 0;
  *bytes = 0;
  for (cJSON *m = a->messages->child; m; m = m->next) {
    size_t size;
    if (message_size(m, &size)) return history_fail(a, "cannot serialize history");
    if (++*count > HISTORY_MAX_MESSAGES)
      return history_fail(a, "history message budget exceeded");
    if (size > HISTORY_MAX_BYTES - *bytes)
      return history_fail(a, "history byte budget exceeded");
    *bytes += size;
  }
  return 0;
}

/* Remove one oldest complete user turn, stopping before the active turn.
 * Incomplete tool sequences and non-user prefixes are never split/evicted. */
static size_t drop_oldest_turn(MotirisAgent *a, cJSON *active,
                               size_t *dropped_bytes) {
  cJSON *first = a->messages->child;
  if (!first || first == active || strcmp(message_role(first), "user")) return 0;
  cJSON *pending = NULL, *last = NULL, *end = NULL;
  for (cJSON *m = first; m && m != active; m = m->next) {
    const char *role = message_role(m);
    if (m != first && !strcmp(role, "user")) { end = m; break; }
    if (pending && strcmp(role, "tool")) return 0;
    if (!strcmp(role, "assistant")) {
      cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
      if (calls && !cJSON_IsArray(calls)) return 0;
      pending = calls ? calls->child : NULL;
    } else if (!strcmp(role, "tool")) {
      if (!pending) return 0;
      pending = pending->next;
    } else if (strcmp(role, "user")) {
      return 0;
    }
    last = m;
    end = m->next;
  }
  if (!last || pending || strcmp(message_role(last), "assistant")) return 0;

  size_t count = 0, bytes = 0;
  for (cJSON *m = first; m != end; m = m->next) {
    size_t size;
    if (message_size(m, &size)) return 0;
    bytes += size;
    count++;
  }
  while (a->messages->child != end)
    cJSON_Delete(cJSON_DetachItemFromArray(a->messages, 0));
  if (a->verbose)
    agent_diagnostic(a, "[motiris] history retention: evicted %zu messages, %zu bytes\n",
                     count, bytes);
  *dropped_bytes = bytes;
  return count;
}

static int history_retain(MotirisAgent *a, cJSON *active) {
  size_t count, bytes;
  if (history_measure(a, &count, &bytes)) return -1;
  while (count > a->history_max_messages || bytes > a->history_max_bytes) {
    size_t dropped_bytes = 0;
    size_t dropped = drop_oldest_turn(a, active, &dropped_bytes);
    if (!dropped)
      return history_fail(a, count > a->history_max_messages
                          ? "history message budget exceeded for active turn"
                          : "history byte budget exceeded for active turn");
    count -= dropped;
    bytes -= dropped_bytes;
  }
  return 0;
}

static int append_message(MotirisAgent *a, cJSON *message) {
  size_t size, count, bytes;
  if (message_size(message, &size)) return history_fail(a, "cannot serialize history");
  if (size > a->history_max_bytes)
    return history_fail(a, "history byte budget exceeded for active turn");
  if (history_measure(a, &count, &bytes)) return -1;
  while (count >= a->history_max_messages ||
         bytes > a->history_max_bytes - size) {
    size_t dropped_bytes = 0;
    size_t dropped = drop_oldest_turn(a, a->run_start, &dropped_bytes);
    if (!dropped)
      return history_fail(a, count >= a->history_max_messages
                          ? "history message budget exceeded for active turn"
                          : "history byte budget exceeded for active turn");
    count -= dropped;
    bytes -= dropped_bytes;
  }
  cJSON_AddItemToArray(a->messages, message);
  return 0;
}

int motiris_history_retain(MotirisAgent *a) {
  if (!a) return -1;
  a->history_error = 0;
  return history_retain(a, NULL);
}

int motiris_set_history_limits(MotirisAgent *a, size_t max_messages,
                               size_t max_bytes) {
  if (!a || !max_messages || max_messages > HISTORY_MAX_MESSAGES ||
      !max_bytes || max_bytes > HISTORY_MAX_BYTES) {
    if (a) history_fail(a, "invalid history limits");
    return -1;
  }
  a->history_max_messages = max_messages;
  a->history_max_bytes = max_bytes;
  return motiris_history_retain(a);
}

int motiris_set_replay_limits(MotirisAgent *a, size_t max_messages,
                              size_t max_bytes) {
  if (!a || !max_messages || max_messages > HISTORY_MAX_MESSAGES ||
      !max_bytes || max_bytes > HISTORY_MAX_BYTES) {
    if (a) history_fail(a, "invalid replay limits");
    return -1;
  }
  a->replay_max_messages = max_messages;
  a->replay_max_bytes = max_bytes;
  return 0;
}

static void add_user_message(MotirisAgent *a, const char *msg, int external) {
  if (external) {
    a->run_start = NULL;
    a->history_error = 0;
  }
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  cJSON_AddStringToObject(m, "content", msg);
  if (append_message(a, m)) cJSON_Delete(m);
}

void motiris_add_user(MotirisAgent *a, const char *msg) {
  add_user_message(a, msg, 1);
}

static int approval_granted(const MotirisAgent *a, int slot) {
  return (a->approval_grants[slot / 8] &
          (unsigned char)(1u << (slot % 8))) != 0;
}

static void set_approval_grant(MotirisAgent *a, int slot, int granted) {
  unsigned char mask = (unsigned char)(1u << (slot % 8));
  if (granted) a->approval_grants[slot / 8] |= mask;
  else a->approval_grants[slot / 8] &= (unsigned char)~mask;
}

void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  if (!a || !a->tools_on || !t || !t->name || a->ntools >= MAX_TOOLS) return;
  int slot = a->ntools++;
  set_approval_grant(a, slot, 0);
  a->tools[slot] = *t;
  /* external per-tool schema (toolscan.c) replaces the embedded one */
  const char *ext = motiris_tool_schema(t->name);
  if (ext) a->tools[slot].parameters = ext;
}

void motiris_unregister_tool(MotirisAgent *a, const char *name) {
  for (int i = 0; i < a->ntools; i++) {
    if (!strcmp(a->tools[i].name, name)) {
      /* Registry slots move left; move only the corresponding grant bits. */
      for (int j = i; j + 1 < a->ntools; j++)
        set_approval_grant(a, j, approval_granted(a, j + 1));
      set_approval_grant(a, a->ntools - 1, 0);
      memmove(&a->tools[i], &a->tools[i + 1],
              sizeof(MotirisTool) * (size_t)(a->ntools - i - 1));
      a->ntools--;
      return;
    }
  }
}

cJSON *motiris_messages(MotirisAgent *a) { return a->messages; }
const cJSON *motiris_turn_start(MotirisAgent *a) {
  return a ? a->run_start : NULL;
}

static const MotirisTool *find_tool(MotirisAgent *a, const char *name) {
  for (int i = 0; i < a->ntools; i++)
    if (!strcmp(a->tools[i].name, name)) return &a->tools[i];
  return NULL;
}

/* goal-check helpers: the goal is the first user message of the session */
static const char *first_user_goal(MotirisAgent *a) {
  for (cJSON *m = a->messages->child; m; m = m->next) {
    cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
    if (!cJSON_IsString(role) || strcmp(role->valuestring, "user")) continue;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
    if (cJSON_IsString(c)) return c->valuestring;
  }
  return "";
}

/* true when the model's reply is just the DONE confirmation (trimmed,
 * case-insensitive, tolerating surrounding dots) */
static int done_reply(const char *s) {
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

/* ---------------- request assembly ---------------- */
static int replay_range_size(cJSON *start, const cJSON *end,
                             size_t *count, size_t *bytes) {
  *count = 0;
  *bytes = 0;
  for (cJSON *m = start; m && m != end; m = m->next) {
    size_t size;
    if (message_size(m, &size)) return -1;
    (*count)++;
    *bytes += size;
  }
  return 0;
}

/* Select a suffix of whole user turns. The active turn is mandatory; older
 * turns are included newest-first only while the independent replay budget
 * fits. This never creates an orphan tool result in a provider request. */
static cJSON *replay_start(MotirisAgent *a, int *ok) {
  *ok = 0;
  cJSON *first = a->messages->child;
  if (!first) { *ok = 1; return NULL; }
  cJSON *start = a->run_start;
  if (!start) {
    for (start = first; start && strcmp(message_role(start), "user");
         start = start->next) {}
    if (!start) start = first;
  }
  size_t count, bytes;
  if (replay_range_size(start, NULL, &count, &bytes)) {
    set_error(a, "cannot serialize replay history", NULL);
    return NULL;
  }
  if (count > a->replay_max_messages || bytes > a->replay_max_bytes) {
    set_error(a, count > a->replay_max_messages
              ? "replay message budget exceeded for active turn"
              : "replay byte budget exceeded for active turn", NULL);
    return NULL;
  }
  while (start != first) {
    cJSON *candidate = NULL;
    for (cJSON *m = first; m != start; m = m->next)
      if (!strcmp(message_role(m), "user")) candidate = m;
    if (!candidate) break;
    size_t extra_count, extra_bytes;
    if (replay_range_size(candidate, start, &extra_count, &extra_bytes)) {
      set_error(a, "cannot serialize replay history", NULL);
      return NULL;
    }
    if (extra_count > a->replay_max_messages - count ||
        extra_bytes > a->replay_max_bytes - bytes)
      break;
    start = candidate;
    count += extra_count;
    bytes += extra_bytes;
  }
  if (a->verbose && start != first) {
    size_t total = 0;
    for (cJSON *m = first; m; m = m->next) total++;
    agent_diagnostic(a, "[motiris] replay retention: sending %zu of %zu messages, %zu bytes\n",
                     count, total, bytes);
  }
  *ok = 1;
  return start;
}

static char *build_request(MotirisAgent *a, const char *model) {
  int replay_ok;
  cJSON *replay = replay_start(a, &replay_ok);
  if (!replay_ok) return NULL;
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "model", model);

  /* messages: [ {role:system}, ...history ] */
  cJSON *msgs = cJSON_CreateArray();
  if (a->system) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "role", "system");
    cJSON_AddStringToObject(s, "content", a->system);
    cJSON_AddItemToArray(msgs, s);
  }
  for (cJSON *m = replay; m; m = m->next)
    cJSON_AddItemToArray(msgs, cJSON_Duplicate(m, 1));
  cJSON_AddItemToObject(req, "messages", msgs);

  /* A pre-registered registry must not bypass a later disabled policy. */
  if (a->tools_on && a->ntools > 0) {
    cJSON *tools = cJSON_CreateArray();
    for (int i = 0; i < a->ntools; i++) {
      const MotirisTool *t = &a->tools[i];
      cJSON *td = cJSON_CreateObject();
      cJSON_AddItemToObject(td, "type", cJSON_CreateString("function"));
      cJSON *fn = cJSON_CreateObject();
      cJSON_AddStringToObject(fn, "name", t->name);
      cJSON_AddStringToObject(fn, "description",
                              t->description ? t->description : "");
      if (t->parameters) {
        cJSON *schema = cJSON_Parse(t->parameters);
        if (schema) cJSON_AddItemToObject(fn, "parameters", schema);
      }
      cJSON_AddItemToObject(td, "function", fn);
      cJSON_AddItemToArray(tools, td);
    }
    cJSON_AddItemToObject(req, "tools", tools);
  }

  cJSON_AddNumberToObject(req, "temperature", 0.2);
  if (*a->reasoning_effort)
    cJSON_AddStringToObject(req, "reasoning_effort", a->reasoning_effort);
  if (a->stream) cJSON_AddTrueToObject(req, "stream");
  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  return body;
}

/* ---------------- response handling ---------------- */
/* Provider response metadata is not part of motiris's replay contract. Keep
 * only the fields accepted by the strict snapshot codec at ingestion;
 * imported snapshots continue to reject unknown fields. */
static void retain_fields(cJSON *object, const char *const *allowed,
                          size_t count) {
  if (!cJSON_IsObject(object)) return;
  for (cJSON *field = object->child; field;) {
    cJSON *next = field->next;
    size_t i = 0;
    if (field->string)
      for (; i < count; i++)
        if (!strcmp(field->string, allowed[i])) break;
    if (!field->string || i == count)
      cJSON_Delete(cJSON_DetachItemViaPointer(object, field));
    field = next;
  }
}

static cJSON *canonical_assistant_message(const cJSON *message) {
  static const char *const message_fields[] = {
    "role", "content", "tool_calls"
  };
  static const char *const call_fields[] = { "id", "type", "function" };
  static const char *const function_fields[] = { "name", "arguments" };
  cJSON *assistant = cJSON_Duplicate(message, 1);
  if (!assistant) return NULL;
  retain_fields(assistant, message_fields,
                sizeof message_fields / sizeof message_fields[0]);
  cJSON *calls = cJSON_GetObjectItemCaseSensitive(assistant, "tool_calls");
  for (cJSON *call = cJSON_IsArray(calls) ? calls->child : NULL;
       call; call = call->next) {
    retain_fields(call, call_fields, sizeof call_fields / sizeof call_fields[0]);
    retain_fields(cJSON_GetObjectItemCaseSensitive(call, "function"),
                  function_fields,
                  sizeof function_fields / sizeof function_fields[0]);
  }
  return assistant;
}

static char *string_result(const char *text) {
  cJSON *j = cJSON_CreateString(text);
  char *printed = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  return printed;
}

static int push_tool_message(MotirisAgent *a, const char *call_id,
                             const char *content) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "tool");
  cJSON_AddStringToObject(m, "tool_call_id", call_id);
  cJSON_AddStringToObject(m, "content", content);
  if (append_message(a, m)) { cJSON_Delete(m); return -1; }
  return 0;
}

/* Keep the history that existed at run entry (including the active user
 * instruction), but discard every assistant/tool/synthetic message from an
 * interrupted run. This also leaves a valid prefix for the next instruction. */
static int cancel_run(MotirisAgent *a, cJSON *run_tail,
                      long tok_in, long tok_out) {
  cJSON *m = run_tail ? run_tail->next : a->messages->child;
  while (m) {
    cJSON *next = m->next;
    cJSON_DetachItemViaPointer(a->messages, m);
    cJSON_Delete(m);
    m = next;
  }
  a->tok_in = tok_in;
  a->tok_out = tok_out;
  set_error(a, "%s", "cancelled");
  return MOTIRIS_RUN_CANCELLED;
}

int motiris_run(MotirisAgent *a) {
  if (a->history_error) return 1;
  free(a->last_error);
  a->last_error = NULL;
  a->run_start = NULL;
  size_t message_count = (size_t)cJSON_GetArraySize(a->messages);
  if (a->compact_enabled && message_count > a->compact_threshold) {
    if (!a->compactor) {
      set_error(a, "%s",
                "context compaction enabled but context-compaction plugin is unavailable");
      return 1;
    }
    if (a->compactor(a, a->compact_keep, a->compact_max_chars,
                     a->compact_ud) < 0) {
      set_error(a, "%s", "context compaction failed; original context retained");
      return 1;
    }
  }
  for (cJSON *m = a->messages->child; m; m = m->next)
    if (!strcmp(message_role(m), "user")) a->run_start = m;
  if (history_retain(a, a->run_start)) return 1;
  cJSON *run_tail = a->messages->child;
  if (run_tail) while (run_tail->next) run_tail = run_tail->next;
  long start_tok_in = a->tok_in, start_tok_out = a->tok_out;
  if (run_cancelled(a)) return cancel_run(a, run_tail, start_tok_in, start_tok_out);
  const char *env_key = getenv("MOTIRIS_API_KEY");
  const char *default_key = a->api_key ? a->api_key : (env_key ? env_key : NULL);
  int np = a->nproviders > 0 ? a->nproviders : 1;
  int confirm_asked = 0;   /* goal-check: one confirmation round, per run */

  /* Reject the whole invalid chain before any provider can perform I/O. */
  for (int pi = 0; pi < np; pi++) {
    const char *key = a->nproviders > 0
        ? a->providers[pi].api_key : default_key;
    if (key && strlen(key) > AUTH_MAX_KEY_BYTES) {
      set_error(a, "authorization header too long", NULL);
      return 1;
    }
  }

  for (unsigned long step = 0;
       a->max_steps == 0 || step < (unsigned long)a->max_steps; step++) {
    if (run_cancelled(a)) return cancel_run(a, run_tail, start_tok_in, start_tok_out);
    char *last_err = NULL;
    cJSON *j = NULL;
    int delta_seen = 0, reasoning_seen = 0;

    /* try providers in order; on transport/parse/api error, fail over */
    for (int pi = 0; pi < np; pi++) {
      const char *model, *url, *key;
      if (a->nproviders > 0) {
        model = a->providers[pi].model;
        url = a->providers[pi].base_url;
        /* A NULL entry key is explicitly keyless: never inherit the default. */
        key = a->providers[pi].api_key;
      } else {
        model = a->model;
        url = a->base_url;
        key = default_key;
      }
      if (a->verbose)
        agent_diagnostic(a, "[motiris] step %lu: %s via %s\n", step + 1, model,
                         a->nproviders > 0 ? pname(a, pi) : "default provider");

      char *body = build_request(a, model);
      if (!body) return 1;

      char auth[AUTH_HEADER_SIZE];
      char *authp = NULL;
      if (key) {
        int n = snprintf(auth, sizeof auth, AUTH_HEADER_PREFIX "%s", key);
        if (n < 0 || (size_t)n >= sizeof auth) {
          free(body);
          free(last_err);
          set_error(a, n < 0 ? "cannot construct authorization header"
                             : "authorization header too long", NULL);
          return 1;
        }
        authp = auth;
      }

      char *err = NULL;
      DeltaForward forward = { a->on_delta, a->delta_ud, 0 };
      ReasoningForward reasoning = {
        a->on_reasoning, a->reasoning_ud, 0
      };
      char *resp = a->stream
          ? motiris_transport_send_stream(
                a->transport, url, authp, body,
                a->on_delta && !confirm_asked ? forward_delta : NULL,
                a->on_delta && !confirm_asked ? &forward : NULL,
                a->on_reasoning && !confirm_asked ? forward_reasoning : NULL,
                a->on_reasoning && !confirm_asked ? &reasoning : NULL,
                a->cancel, a->cancel_ud, &err)
          : motiris_transport_send(a->transport, url, authp, body,
                                   a->cancel, a->cancel_ud, &err);
      delta_seen = forward.seen;
      reasoning_seen = reasoning.seen;
      free(body);
      if (run_cancelled(a)) {
        free(resp);
        free(err);
        free(last_err);
        return cancel_run(a, run_tail, start_tok_in, start_tok_out);
      }
      if (!resp) {
        free(last_err);
        last_err = malloc((err ? strlen(err) : 0) + 32);
        sprintf(last_err, "transport error: %s", err ? err : "(unknown)");
        free(err);
        if (a->verbose && a->nproviders > 0)
          agent_diagnostic(a, "[motiris] provider %s failed: %s\n",
                           pname(a, pi), last_err);
        /* Any visible provider output cannot be mixed with a fallback. */
        if (delta_seen || reasoning_seen) break;
        continue;
      }
      if (a->verbose) {
        agent_diagnostic(a, "[motiris] response: %.200s%s\n", resp,
                         strlen(resp) > 200 ? "..." : "");
      }

      j = cJSON_Parse(resp);
      free(resp);
      if (!j) {
        free(last_err);
        last_err = sdup("cannot parse model response");
        if (delta_seen || reasoning_seen) break;
        continue;
      }
      cJSON *errj = cJSON_GetObjectItemCaseSensitive(j, "error");
      if (errj) {
        const char *em = cJSON_IsObject(errj)
            ? cJSON_GetStringValue(
                  cJSON_GetObjectItemCaseSensitive(errj, "message"))
            : cJSON_GetStringValue(errj);
        free(last_err);
        last_err = malloc((em ? strlen(em) : 0) + 16);
        sprintf(last_err, "api error: %s", em ? em : "(unknown)");
        cJSON_Delete(j);
        j = NULL;
        if (a->verbose && a->nproviders > 0)
          agent_diagnostic(a, "[motiris] provider %s failed: %s\n",
                           pname(a, pi), last_err);
        if (delta_seen || reasoning_seen) break;
        continue;
      }
      /* accumulate usage/token stats from this response */
      cJSON *usage = cJSON_GetObjectItemCaseSensitive(j, "usage");
      if (cJSON_IsObject(usage)) {
        cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (cJSON_IsNumber(pt)) a->tok_in += (long)pt->valuedouble;
        if (cJSON_IsNumber(ct)) a->tok_out += (long)ct->valuedouble;
      }
      break; /* got a usable response */
    }

    if (!j) {
      set_error(a, "%s", last_err ? last_err : "all providers failed");
      free(last_err);
      return 1;
    }
    free(last_err);
    last_err = NULL;

    cJSON *choice = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
    if (!choice) {
      set_error(a, "response has no choices", NULL);
      cJSON_Delete(j);
      return 1;
    }
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (!msg) {
      set_error(a, "response message missing", NULL);
      cJSON_Delete(j);
      return 1;
    }

    if (run_cancelled(a)) {
      free(last_err);
      cJSON_Delete(j);
      return cancel_run(a, run_tail, start_tok_in, start_tok_out);
    }

    /* Keep one canonical assistant representation for replay/checkpoints. */
    cJSON *assistant = canonical_assistant_message(msg);
    if (!assistant || append_message(a, assistant)) {
      cJSON_Delete(assistant);
      cJSON_Delete(j);
      return 1;
    }

    cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    const char *finish = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));

    if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
      for (cJSON *tc = calls->child; tc; tc = tc->next) {
        if (run_cancelled(a)) {
          free(last_err);
          cJSON_Delete(j);
          return cancel_run(a, run_tail, start_tok_in, start_tok_out);
        }
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(fn, "name"));
        const char *args = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(fn, "arguments"));
        const char *id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(tc, "id"));
        if (!name) continue;

        const MotirisTool *t = a->tools_on ? find_tool(a, name) : NULL;
        char *result;
        if (!t) {
          result = string_result("error: unknown tool (not registered)");
        } else {
          int slot = (int)(t - a->tools);
          int allowed = !a->approval_required || approval_granted(a, slot);
          if (!allowed && !a->approval) {
            result = string_result(
                "error: approval unavailable (no callback installed)");
            if (push_tool_message(a, id ? id : "", result)) {
              free(result);
              cJSON_Delete(j);
              return MOTIRIS_RUN_ERROR;
            }
            free(result);
            set_error(a, "%s",
                      "approval required but no approval callback is installed");
            cJSON_Delete(j);
            return MOTIRIS_RUN_ERROR;
          }
          if (!allowed) {
            MotirisApprovalDecision decision = a->approval(
                name, args ? args : "{}", a->approval_ud);
            if (decision == MOTIRIS_APPROVAL_ALLOW_SESSION) {
              set_approval_grant(a, slot, 1);
              allowed = 1;
            } else if (decision == MOTIRIS_APPROVAL_ALLOW_ONCE) {
              allowed = 1;
            }
          }
          if (!allowed) {
            result = string_result("error: tool call denied by approval policy");
          } else {
            if (a->verbose) agent_diagnostic(a, "[motiris] tool call: %s(%s)\n",
                                             name, args ? args : "");
            result = t->call(args ? args : "{}", t->ud);
            if (!result) result = string_result(
                "error: tool returned nothing");
            for (int hi = 0; hi < a->ntool_hooks; hi++)
              a->tool_hooks[hi].cb(name, args ? args : "{}", result,
                                   a->tool_hooks[hi].ud);
          }
        }
        if (push_tool_message(a, id ? id : "", result)) {
          free(result);
          cJSON_Delete(j);
          return 1;
        }
        free(result);
      }
      cJSON_Delete(j);
      if (run_cancelled(a)) {
        free(last_err);
        return cancel_run(a, run_tail, start_tok_in, start_tok_out);
      }
      continue; /* next round */
    }

    /* final answer */
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(msg, "content"));
    /* goal-check: a bare DONE reply to the confirmation ends silently
     * (the real answer was already printed in the previous round) */
    if (a->goal_check && confirm_asked && content && done_reply(content)) {
      cJSON_Delete(j);
      return 0;
    }
    if (content && *content) {
      if (!delta_seen && a->on_final) a->on_final(content, a->final_ud);
      else if (!delta_seen && a->print_answer) printf("%s\n", content);
    } else if (!finish || strcmp(finish, "stop"))
      agent_diagnostic(a, "[motiris] warning: model stopped without output\n");
    if (a->goal_check && !confirm_asked) {
      /* one confirmation round: ask the model whether the goal is met;
       * it may reply DONE or keep working (tools included), bounded by
       * max_steps. Appended to THIS agent's messages only. */
      const char *goal = first_user_goal(a);
      size_t n = strlen(goal) + 120;
      char *confirm = malloc(n);
      snprintf(confirm, n,
               "Goal check: <%s>. If the goal is fully achieved reply "
               "exactly DONE; otherwise keep working (you may call tools).",
               goal);
      add_user_message(a, confirm, 0);
      free(confirm);
      if (a->history_error) { cJSON_Delete(j); return 1; }
      confirm_asked = 1;
      cJSON_Delete(j);
      continue;
    }
    if (run_cancelled(a)) {
      free(last_err);
      cJSON_Delete(j);
      return cancel_run(a, run_tail, start_tok_in, start_tok_out);
    }
    cJSON_Delete(j);
    free(last_err);
    return 0;
  }

  if (run_cancelled(a)) return cancel_run(a, run_tail, start_tok_in, start_tok_out);
  set_error(a, "max steps reached", NULL);
  return 1;
}