/* mote.h - public API of mote, a mote of an agent framework.
 * MIT License (c) 2026 Shen-KD
 */
#ifndef MOTE_H
#define MOTE_H

#include <stddef.h>

/* JSON: vendored cJSON (MIT, DaveGamble/cJSON) - single-file, tiny. */
#include <cJSON.h>

/* ---- Agent ---- */
typedef struct MoteAgent MoteAgent;

typedef struct MoteTool {
  const char *name;          /* tool id sent to model */
  const char *description;   /* shown to model */
  const char *parameters;    /* JSON-schema-ish string, or NULL */
  char *(*call)(const char *args_json, void *ud);  /* returns malloc'd JSON */
  void *ud;
} MoteTool;

MoteAgent *mote_new(void);
void mote_free(MoteAgent *a);
void mote_set_model(MoteAgent *a, const char *model);
void mote_set_base_url(MoteAgent *a, const char *url);   /* full chat endpoint */
void mote_set_api_key(MoteAgent *a, const char *key);
void mote_set_system(MoteAgent *a, const char *sys);
void mote_add_user(MoteAgent *a, const char *msg);
void mote_register_tool(MoteAgent *a, const MoteTool *t); /* copied */
void mote_unregister_tool(MoteAgent *a, const char *name); /* runtime unplug */
cJSON *mote_messages(MoteAgent *a);                        /* session log */

/* ---- plugins: runtime-loadable shared objects ---- */
typedef struct MotePluginApi {
  int version;                                   /* MOTE_PLUGIN_API_VERSION */
  void (*register_tool)(MoteAgent *a, const MoteTool *t);
  void (*unregister_tool)(MoteAgent *a, const char *name);
} MotePluginApi;

#define MOTE_PLUGIN_API_VERSION 1

/* A plugin shared object exports:
 *   int mote_plugin_init(MoteAgent *a, const MotePluginApi *api);
 * Returns 0 on success. Called at load time. */
typedef int (*mote_plugin_init_fn)(MoteAgent *a, const MotePluginApi *api);

int mote_load_plugins(MoteAgent *a, const char *dir);  /* 0 on success */
void mote_set_transport(MoteAgent *a, const char *name);  /* "curl"|"echo" */
void mote_set_max_steps(MoteAgent *a, int n);
void mote_set_verbose(MoteAgent *a, int on);
int  mote_run(MoteAgent *a);   /* 0 on success */
const char *mote_last_error(MoteAgent *a);

/* built-in tools */
void mote_register_core_tools(MoteAgent *a);

#endif /* MOTE_H */