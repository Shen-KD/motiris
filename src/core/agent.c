/* agent.c - the motiris agent loop.
 *
 *   assemble request -> transport -> parse -> tool_calls?
 *        ^                                          |
 *        |                   append tool results  <-+ (execute tools)
 *        +------------ final answer printed, done  (finish_reason == stop)
 *
 * Messages are kept as a cJSON array; each round appends. The loop is
 * bounded by max_steps so a misbehaving model cannot loop forever.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 128
#define HISTORY_MAX_MESSAGES 256
/* Host tools can return bounded, escaped multi-MiB results. The live cap is
 * an allocation ceiling, not a preallocation; MCU embedders can set it lower. */
#define HISTORY_MAX_BYTES (16 * 1024 * 1024)
#define DEF_BASE_URL "https://api.openai.com/v1/chat/completions"

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
  int stream;
  void (*on_delta)(const char *, void *);
  void *delta_ud;
  MotirisCancelCallback *cancel;
  void *cancel_ud;
  int tools_on, plugins_on;
  int goal_check;
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

/* transport.c */
char *motiris_transport_send(const char *backend, const char *url,
                             const char *auth, const char *body,
                             MotirisCancelCallback *cancel, void *cancel_ud,
                             char **err);
char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud, MotirisCancelCallback *cancel,
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
  a->max_steps = 10;
  a->history_max_messages = env_limit("MOTIRIS_HISTORY_MAX_MESSAGES",
                                      HISTORY_MAX_MESSAGES, HISTORY_MAX_MESSAGES);
  a->history_max_bytes = env_limit("MOTIRIS_HISTORY_MAX_BYTES",
                                   HISTORY_MAX_BYTES, HISTORY_MAX_BYTES);
  a->replay_max_messages = env_limit("MOTIRIS_REPLAY_MAX_MESSAGES",
                                     HISTORY_MAX_MESSAGES, HISTORY_MAX_MESSAGES);
  a->replay_max_bytes = env_limit("MOTIRIS_REPLAY_MAX_BYTES",
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
void motiris_set_stream_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                           void *ud) { a->on_delta = cb; a->delta_ud = ud; }
void motiris_set_cancel_cb(MotirisAgent *a, MotirisCancelCallback *cb, void *ud) {
  a->cancel = cb;
  a->cancel_ud = ud;
}
static int run_cancelled(const MotirisAgent *a) {
  return a->cancel && a->cancel(a->cancel_ud);
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
void motiris_set_max_steps(MotirisAgent *a, int n) { a->max_steps = n; }
void motiris_set_verbose(MotirisAgent *a, int on) { a->verbose = on; }
void motiris_set_goal_check(MotirisAgent *a, int on) { a->goal_check = !!on; }

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
    fprintf(stderr, "[motiris] history retention: evicted %zu messages, %zu bytes\n",
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

void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  if (!t || !t->name || a->ntools >= MAX_TOOLS) return;
  a->tools[a->ntools++] = *t;
  /* external per-tool schema (toolscan.c) replaces the embedded one */
  const char *ext = motiris_tool_schema(t->name);
  if (ext) a->tools[a->ntools - 1].parameters = ext;
}

void motiris_unregister_tool(MotirisAgent *a, const char *name) {
  for (int i = 0; i < a->ntools; i++) {
    if (!strcmp(a->tools[i].name, name)) {
      memmove(&a->tools[i], &a->tools[i + 1],
              sizeof(MotirisTool) * (size_t)(a->ntools - i - 1));
      a->ntools--;
      return;
    }
  }
}

cJSON *motiris_messages(MotirisAgent *a) { return a->messages; }

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
    fprintf(stderr, "[motiris] replay retention: sending %zu of %zu messages, %zu bytes\n",
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

  /* tools: only when registered */
  if (a->ntools > 0) {
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
  if (a->stream) cJSON_AddTrueToObject(req, "stream");
  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  return body;
}

/* ---------------- response handling ---------------- */
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
  for (cJSON *m = a->messages->child; m; m = m->next)
    if (!strcmp(message_role(m), "user")) a->run_start = m;
  if (history_retain(a, a->run_start)) return 1;
  cJSON *run_tail = a->messages->child;
  if (run_tail) while (run_tail->next) run_tail = run_tail->next;
  long start_tok_in = a->tok_in, start_tok_out = a->tok_out;
  if (run_cancelled(a)) return cancel_run(a, run_tail, start_tok_in, start_tok_out);
  const char *env_key = getenv("MOTIRIS_API_KEY");
  const char *fallback_key = a->api_key ? a->api_key : (env_key ? env_key : NULL);
  int np = a->nproviders > 0 ? a->nproviders : 1;
  int confirm_asked = 0;   /* goal-check: one confirmation round, per run */

  for (int step = 0; step < a->max_steps; step++) {
    if (run_cancelled(a)) return cancel_run(a, run_tail, start_tok_in, start_tok_out);
    char *last_err = NULL;
    cJSON *j = NULL;

    /* try providers in order; on transport/parse/api error, fail over */
    for (int pi = 0; pi < np; pi++) {
      const char *model, *url, *key;
      if (np > 1) {
        model = a->providers[pi].model;
        url = a->providers[pi].base_url;
        key = a->providers[pi].api_key
            ? a->providers[pi].api_key : fallback_key;
      } else {
        model = a->model;
        url = a->base_url;
        key = fallback_key;
      }
      if (a->verbose)
        fprintf(stderr, "[motiris] step %d: %s via %s\n", step + 1, model,
                np > 1 ? pname(a, pi) : "default provider");

      char *body = build_request(a, model);
      if (!body) return 1;

      char auth[512];
      char *authp = NULL;
      if (key) {
        size_t n = snprintf(auth, sizeof auth,
                            "Authorization: Bearer %s", key);
        if (n < sizeof auth) authp = auth;
      }

      char *err = NULL;
      char *resp = a->stream
          ? motiris_transport_send_stream(a->transport, url, authp, body,
                                          a->on_delta, a->delta_ud,
                                          a->cancel, a->cancel_ud, &err)
          : motiris_transport_send(a->transport, url, authp, body,
                                   a->cancel, a->cancel_ud, &err);
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
        if (a->verbose && np > 1)
          fprintf(stderr, "[motiris] provider %s failed: %s\n",
                  pname(a, pi), last_err);
        continue;
      }
      if (a->verbose) {
        fprintf(stderr, "[motiris] response: %.200s%s\n", resp,
                strlen(resp) > 200 ? "..." : "");
      }

      j = cJSON_Parse(resp);
      free(resp);
      if (!j) {
        free(last_err);
        last_err = sdup("cannot parse model response");
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
        if (a->verbose && np > 1)
          fprintf(stderr, "[motiris] provider %s failed: %s\n",
                  pname(a, pi), last_err);
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

    /* keep assistant message in history (may carry tool_calls) */
    cJSON *assistant = cJSON_Duplicate(msg, 1);
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

        const MotirisTool *t = find_tool(a, name);
        char *result;
        if (!t) {
          result = string_result("error: unknown tool (not registered)");
        } else {
          if (a->verbose) fprintf(stderr, "[motiris] tool call: %s(%s)\n",
                                          name, args ? args : "");
                  result = t->call(args ? args : "{}", t->ud);
                  if (!result) result = string_result(
                      "error: tool returned nothing");
                  for (int hi = 0; hi < a->ntool_hooks; hi++)
                    a->tool_hooks[hi].cb(name, args ? args : "{}", result,
                                         a->tool_hooks[hi].ud);
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
    if (content && *content) printf("%s\n", content);
    else if (!finish || strcmp(finish, "stop"))
      fprintf(stderr, "[motiris] warning: model stopped without output\n");
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