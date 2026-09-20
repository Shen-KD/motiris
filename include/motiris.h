/* motiris.h - public API of motiris, the messenger-goddess agent.
 * Iris (goddess of the rainbow, messenger of the gods) as a mote:
 * a featherweight C11 agent framework. MIT License (c) 2026 Shen-KD.
 */
#ifndef MOTIRIS_H
#define MOTIRIS_H

#include <stddef.h>

/* JSON: vendored cJSON (MIT, DaveGamble/cJSON) - single-file, tiny. */
#include <cJSON.h>

/* ---- Agent ---- */
typedef struct MotirisAgent MotirisAgent;

typedef struct MotirisTool {
  const char *name;          /* tool id sent to model */
  const char *description;   /* shown to model */
  const char *parameters;    /* JSON-schema-ish string, or NULL */
  char *(*call)(const char *args_json, void *ud);  /* returns malloc'd JSON */
  void *ud;
} MotirisTool;

MotirisAgent *motiris_new(void);
void motiris_free(MotirisAgent *a);
void motiris_set_model(MotirisAgent *a, const char *model);
void motiris_set_base_url(MotirisAgent *a, const char *url);   /* full chat endpoint */
void motiris_set_api_key(MotirisAgent *a, const char *key);
void motiris_set_system(MotirisAgent *a, const char *sys);
void motiris_add_user(MotirisAgent *a, const char *msg);
void motiris_register_tool(MotirisAgent *a, const MotirisTool *t); /* copied */
void motiris_unregister_tool(MotirisAgent *a, const char *name); /* runtime unplug */
cJSON *motiris_messages(MotirisAgent *a);                        /* session log */

/* ---- plugins: runtime-loadable shared objects ---- */
typedef struct MotirisPluginApi {
  int version;                                   /* MOTIRIS_PLUGIN_API_VERSION */
  void (*register_tool)(MotirisAgent *a, const MotirisTool *t);
  void (*unregister_tool)(MotirisAgent *a, const char *name);
} MotirisPluginApi;

#define MOTIRIS_PLUGIN_API_VERSION 1

/* A plugin shared object exports:
 *   int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api);
 * Returns 0 on success. Called at load time. */
typedef int (*motiris_plugin_init_fn)(MotirisAgent *a, const MotirisPluginApi *api);

int motiris_load_plugins(MotirisAgent *a, const char *dir);  /* 0 on success */
void motiris_set_transport(MotirisAgent *a, const char *name);  /* "curl"|"echo" */
void motiris_set_max_steps(MotirisAgent *a, int n);
void motiris_set_plugins_enabled(MotirisAgent *a, int on);
void motiris_set_tools_enabled(MotirisAgent *a, int on);
void motiris_set_plugin_dir(MotirisAgent *a, const char *dir);
int  motiris_plugins_enabled(MotirisAgent *a);
const char *motiris_plugin_dir(MotirisAgent *a);
void motiris_set_verbose(MotirisAgent *a, int on);
void motiris_set_stream(MotirisAgent *a, int on);      /* SSE streaming */
void motiris_set_stream_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                           void *ud);   /* per-token sink (default stdout) */
int  motiris_run(MotirisAgent *a);   /* 0 on success */
const char *motiris_last_error(MotirisAgent *a);

/* built-in tools */
void motiris_register_core_tools(MotirisAgent *a);

/* ---- centralized config: $HOME/.motiris/{env,config.json} ---- */
void motiris_load_env(void);                  /* call before new() */
void motiris_apply_config(MotirisAgent *a);   /* defaults, CLI wins */
int  motiris_init_config(void);               /* write templates */

#endif /* MOTIRIS_H */