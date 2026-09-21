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
  int tools_on, plugins_on;
  char *plugin_dir;
  /* tool-call observers (multi-slot; plugins and repl can both watch) */
  struct { MotirisToolHook *cb; void *ud; } tool_hooks[8];
  int ntool_hooks;
  long tok_in, tok_out;
  MotirisProvider providers[8];
  int nproviders;
  char *last_error;
};

/* transport.c */
char *motiris_transport_send(const char *backend, const char *url,
                             const char *auth, const char *body, char **err);
char *motiris_transport_send_stream(const char *backend, const char *url,
                                    const char *auth, const char *body,
                                    void (*on_delta)(const char *, void *),
                                    void *ud, char **err);

/* tools.c */
int motiris_register_core_tools_count(void);
const MotirisTool *motiris_core_tools(void);

void motiris_register_core_tools(MotirisAgent *a) {
  if (!a->tools_on) return;
  for (int i = 0; i < motiris_register_core_tools_count(); i++)
    motiris_register_tool(a, &motiris_core_tools()[i]);
}

static char *sdup(const char *s) { return s ? strdup(s) : NULL; }

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
  d->name = p->name ? strdup(p->name) : NULL;
  d->model = strdup(p->model);
  d->base_url = strdup(p->base_url);
  d->api_key = p->api_key ? strdup(p->api_key) : NULL;
}
int motiris_provider_count(MotirisAgent *a) { return a->nproviders; }

static const char *pname(const MotirisAgent *a, int pi) {
  return a->providers[pi].name ? a->providers[pi].name : a->providers[pi].model;
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
  a->tools_on = 1;
  a->plugins_on = 1;
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
void motiris_set_stream(MotirisAgent *a, int on) { a->stream = !!on; }
void motiris_set_stream_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                           void *ud) { a->on_delta = cb; a->delta_ud = ud; }
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

void motiris_add_user(MotirisAgent *a, const char *msg) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  cJSON_AddStringToObject(m, "content", msg);
  cJSON_AddItemToArray(a->messages, m);
}

void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  if (!t || !t->name || a->ntools >= MAX_TOOLS) return;
  a->tools[a->ntools++] = *t;
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

/* ---------------- request assembly ---------------- */
static char *build_request(MotirisAgent *a, const char *model) {
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
  for (cJSON *m = a->messages->child; m; m = m->next)
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
static void push_tool_message(MotirisAgent *a, const char *call_id,
                              const char *content) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "tool");
  cJSON_AddStringToObject(m, "tool_call_id", call_id);
  cJSON_AddStringToObject(m, "content", content);
  cJSON_AddItemToArray(a->messages, m);
}

int motiris_run(MotirisAgent *a) {
  const char *env_key = getenv("MOTIRIS_API_KEY");
  const char *fallback_key = a->api_key ? a->api_key : (env_key ? env_key : NULL);
  int np = a->nproviders > 0 ? a->nproviders : 1;

  for (int step = 0; step < a->max_steps; step++) {
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
      if (!body) { set_error(a, "request assembly failed", NULL); return 1; }

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
                                          a->on_delta, a->delta_ud, &err)
          : motiris_transport_send(a->transport, url, authp, body, &err);
      free(body);
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
        last_err = strdup("cannot parse model response");
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

    /* keep assistant message in history (may carry tool_calls) */
    cJSON_AddItemToArray(a->messages, cJSON_Duplicate(msg, 1));

    cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    const char *finish = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));

    if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
      for (cJSON *tc = calls->child; tc; tc = tc->next) {
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
          result = cJSON_PrintUnformatted(cJSON_CreateString(
              "error: unknown tool (not registered)"));
        } else {
          if (a->verbose) fprintf(stderr, "[motiris] tool call: %s(%s)\n",
                                          name, args ? args : "");
                  result = t->call(args ? args : "{}", t->ud);
                  if (!result) result = cJSON_PrintUnformatted(
                      cJSON_CreateString("error: tool returned nothing"));
                  for (int hi = 0; hi < a->ntool_hooks; hi++)
                    a->tool_hooks[hi].cb(name, args ? args : "{}", result,
                                         a->tool_hooks[hi].ud);
        }
        push_tool_message(a, id ? id : "", result);
        free(result);
      }
      cJSON_Delete(j);
      continue; /* next round */
    }

    /* final answer */
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(msg, "content"));
    if (content && *content) printf("%s\n", content);
    else if (!finish || strcmp(finish, "stop"))
      fprintf(stderr, "[motiris] warning: model stopped without output\n");
    cJSON_Delete(j);
    return 0;
  }

  set_error(a, "max steps reached", NULL);
  return 1;
}