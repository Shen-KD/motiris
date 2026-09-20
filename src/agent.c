/* agent.c - the mote agent loop.
 *
 *   assemble request -> transport -> parse -> tool_calls?
 *        ^                                          |
 *        |                   append tool results  <-+ (execute tools)
 *        +------------ final answer printed, done  (finish_reason == stop)
 *
 * Messages are kept as a cJSON array; each round appends. The loop is
 * bounded by max_steps so a misbehaving model cannot loop forever.
 */
#include "mote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 64
#define DEF_BASE_URL "https://api.openai.com/v1/chat/completions"

struct MoteAgent {
  char *model;
  char *base_url;
  char *api_key;
  char *system;
  cJSON *messages;         /* array of OpenAI message objects */
  MoteTool tools[MAX_TOOLS];
  int ntools;
  char transport[16];
  int max_steps;
  int verbose;
  char *last_error;
};

/* transport.c */
char *mote_transport_send(const char *backend, const char *url,
                          const char *auth, const char *body, char **err);

/* tools.c */
int mote_register_core_tools_count(void);
const MoteTool *mote_core_tools(void);

void mote_register_core_tools(MoteAgent *a) {
  for (int i = 0; i < mote_register_core_tools_count(); i++)
    mote_register_tool(a, &mote_core_tools()[i]);
}

static char *sdup(const char *s) { return s ? strdup(s) : NULL; }

MoteAgent *mote_new(void) {
  MoteAgent *a = calloc(1, sizeof *a);
  a->base_url = sdup(DEF_BASE_URL);
  const char *m = getenv("MOTE_MODEL");
  a->model = sdup(m && *m ? m : "gpt-4o-mini");
  a->messages = cJSON_CreateArray();
  strcpy(a->transport, "curl");
  a->max_steps = 10;
  return a;
}

void mote_free(MoteAgent *a) {
  if (!a) return;
  free(a->model);
  free(a->base_url);
  free(a->api_key);
  free(a->system);
  free(a->last_error);
  cJSON_Delete(a->messages);
  free(a);
}

void mote_set_model(MoteAgent *a, const char *m) {
  free(a->model); a->model = sdup(m);
}
void mote_set_base_url(MoteAgent *a, const char *u) {
  free(a->base_url); a->base_url = sdup(u);
}
void mote_set_api_key(MoteAgent *a, const char *k) {
  free(a->api_key); a->api_key = sdup(k);
}
void mote_set_transport(MoteAgent *a, const char *n) {
  snprintf(a->transport, sizeof a->transport, "%s", n ? n : "curl");
}
void mote_set_max_steps(MoteAgent *a, int n) { a->max_steps = n; }
void mote_set_verbose(MoteAgent *a, int on) { a->verbose = on; }

const char *mote_last_error(MoteAgent *a) {
  static const char none[] = "";
  return a->last_error ? a->last_error : none;
}

static void set_error(MoteAgent *a, const char *msg, const char *arg) {
  free(a->last_error);
  size_t n = strlen(msg) + (arg ? strlen(arg) : 0) + 2;
  a->last_error = malloc(n);
  snprintf(a->last_error, n, msg, arg ? arg : "");
}

void mote_set_system(MoteAgent *a, const char *sys) {
  free(a->system);
  a->system = sdup(sys);
}

void mote_add_user(MoteAgent *a, const char *msg) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  cJSON_AddStringToObject(m, "content", msg);
  cJSON_AddItemToArray(a->messages, m);
}

void mote_register_tool(MoteAgent *a, const MoteTool *t) {
  if (!t || !t->name || a->ntools >= MAX_TOOLS) return;
  a->tools[a->ntools++] = *t;
}

void mote_unregister_tool(MoteAgent *a, const char *name) {
  for (int i = 0; i < a->ntools; i++) {
    if (!strcmp(a->tools[i].name, name)) {
      memmove(&a->tools[i], &a->tools[i + 1],
              sizeof(MoteTool) * (size_t)(a->ntools - i - 1));
      a->ntools--;
      return;
    }
  }
}

cJSON *mote_messages(MoteAgent *a) { return a->messages; }

static const MoteTool *find_tool(MoteAgent *a, const char *name) {
  for (int i = 0; i < a->ntools; i++)
    if (!strcmp(a->tools[i].name, name)) return &a->tools[i];
  return NULL;
}

/* ---------------- request assembly ---------------- */
static char *build_request(MoteAgent *a) {
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "model", a->model);

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
      const MoteTool *t = &a->tools[i];
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
  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  return body;
}

/* ---------------- response handling ---------------- */
static void push_tool_message(MoteAgent *a, const char *call_id,
                              const char *content) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "tool");
  cJSON_AddStringToObject(m, "tool_call_id", call_id);
  cJSON_AddStringToObject(m, "content", content);
  cJSON_AddItemToArray(a->messages, m);
}

int mote_run(MoteAgent *a) {
  const char *env_key = getenv("MOTE_API_KEY");
  const char *key = a->api_key ? a->api_key : (env_key ? env_key : NULL);

  for (int step = 0; step < a->max_steps; step++) {
    if (a->verbose) fprintf(stderr, "[mote] step %d: requesting model %s\n",
                            step + 1, a->model);

    char *body = build_request(a);
    if (!body) { set_error(a, "request assembly failed", NULL); return 1; }

    char auth[512];
    char *authp = NULL;
    if (key) {
      size_t n = snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
      if (n < sizeof auth) authp = auth;
    }

    char *err = NULL;
    char *resp = mote_transport_send(a->transport, a->base_url, authp,
                                     body, &err);
    free(body);
    if (!resp) {
      set_error(a, err ? "transport error: %s" : "transport error", err);
      free(err);
      return 1;
    }
    if (a->verbose) {
      fprintf(stderr, "[mote] response: %.200s%s\n", resp,
              strlen(resp) > 200 ? "..." : "");
    }

    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) { set_error(a, "cannot parse model response", NULL); return 1; }

    /* error body? {"error": {...}} */
    cJSON *errj = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (errj) {
      const char *em = cJSON_IsObject(errj)
          ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(errj, "message"))
          : cJSON_GetStringValue(errj);
      set_error(a, "api error: %s", em ? em : "(unknown)");
      cJSON_Delete(j);
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

        const MoteTool *t = find_tool(a, name);
        char *result;
        if (!t) {
          result = cJSON_PrintUnformatted(cJSON_CreateString(
              "error: unknown tool (not registered)"));
        } else {
          if (a->verbose) fprintf(stderr, "[mote] tool call: %s(%s)\n",
                                  name, args ? args : "");
          result = t->call(args ? args : "{}", t->ud);
          if (!result) result = cJSON_PrintUnformatted(
              cJSON_CreateString("error: tool returned nothing"));
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
      fprintf(stderr, "[mote] warning: model stopped without output\n");
    cJSON_Delete(j);
    return 0;
  }

  set_error(a, "max steps reached", NULL);
  return 1;
}