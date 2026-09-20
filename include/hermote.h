/* hermote.h - public API of hermote, a hermote of an agent framework.
 * MIT License (c) 2026 Shen-KD
 */
#ifndef HERMOTE_H
#define HERMOTE_H

#include <stddef.h>

/* JSON: vendored cJSON (MIT, DaveGamble/cJSON) - single-file, tiny. */
#include <cJSON.h>

/* ---- Agent ---- */
typedef struct HermoteAgent HermoteAgent;

typedef struct HermoteTool {
  const char *name;          /* tool id sent to model */
  const char *description;   /* shown to model */
  const char *parameters;    /* JSON-schema-ish string, or NULL */
  char *(*call)(const char *args_json, void *ud);  /* returns malloc'd JSON */
  void *ud;
} HermoteTool;

HermoteAgent *hermote_new(void);
void hermote_free(HermoteAgent *a);
void hermote_set_model(HermoteAgent *a, const char *model);
void hermote_set_base_url(HermoteAgent *a, const char *url);   /* full chat endpoint */
void hermote_set_api_key(HermoteAgent *a, const char *key);
void hermote_set_system(HermoteAgent *a, const char *sys);
void hermote_add_user(HermoteAgent *a, const char *msg);
void hermote_register_tool(HermoteAgent *a, const HermoteTool *t); /* copied */
void hermote_unregister_tool(HermoteAgent *a, const char *name); /* runtime unplug */
cJSON *hermote_messages(HermoteAgent *a);                        /* session log */

/* ---- plugins: runtime-loadable shared objects ---- */
typedef struct HermotePluginApi {
  int version;                                   /* HERMOTE_PLUGIN_API_VERSION */
  void (*register_tool)(HermoteAgent *a, const HermoteTool *t);
  void (*unregister_tool)(HermoteAgent *a, const char *name);
} HermotePluginApi;

#define HERMOTE_PLUGIN_API_VERSION 1

/* A plugin shared object exports:
 *   int hermote_plugin_init(HermoteAgent *a, const HermotePluginApi *api);
 * Returns 0 on success. Called at load time. */
typedef int (*hermote_plugin_init_fn)(HermoteAgent *a, const HermotePluginApi *api);

int hermote_load_plugins(HermoteAgent *a, const char *dir);  /* 0 on success */
void hermote_set_transport(HermoteAgent *a, const char *name);  /* "curl"|"echo" */
void hermote_set_max_steps(HermoteAgent *a, int n);
void hermote_set_verbose(HermoteAgent *a, int on);
int  hermote_run(HermoteAgent *a);   /* 0 on success */
const char *hermote_last_error(HermoteAgent *a);

/* built-in tools */
void hermote_register_core_tools(HermoteAgent *a);

#endif /* HERMOTE_H */