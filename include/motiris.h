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
void motiris_set_model(MotirisAgent *a, const char *m);
const char *motiris_model(MotirisAgent *a);         /* current model id */
int  motiris_tool_count(MotirisAgent *a);           /* registered tools */
const char *motiris_tool_name(MotirisAgent *a, int i); /* or NULL */
void motiris_set_base_url(MotirisAgent *a, const char *u);
void motiris_set_api_key(MotirisAgent *a, const char *k);
void motiris_set_system(MotirisAgent *a, const char *sys);
void motiris_add_user(MotirisAgent *a, const char *msg);
/* Per-agent bounded live history and independent provider replay selection.
 * Limits accept 1..256 messages and 1..16 MiB; retention evicts only complete
 * oldest user turns. These APIs return 0, or -1 with motiris_last_error(). */
int  motiris_set_history_limits(MotirisAgent *a, size_t max_messages,
                                size_t max_bytes);
int  motiris_set_replay_limits(MotirisAgent *a, size_t max_messages,
                               size_t max_bytes);
/* Reapply live limits after a backend imports directly into motiris_messages. */
int  motiris_history_retain(MotirisAgent *a);

/* ---- multi-provider fallback ---- */
typedef struct MotirisProvider {
  const char *name;
  const char *model;
  const char *base_url;
  const char *api_key;   /* NULL is keyless; never inherits the Agent key */
} MotirisProvider;
void motiris_add_provider(MotirisAgent *a, const MotirisProvider *p); /* copied */
int  motiris_provider_count(MotirisAgent *a);
void motiris_register_tool(MotirisAgent *a, const MotirisTool *t); /* copied */
void motiris_unregister_tool(MotirisAgent *a, const char *name); /* runtime unplug */
cJSON *motiris_messages(MotirisAgent *a);                        /* session log */

/* ---- plugins: runtime-loadable shared objects ---- */
/* tool-invocation observer: called after each tool executes */
typedef void (MotirisToolHook)(const char *name, const char *args,
                               const char *result, void *ud);

typedef struct MotirisPluginApi {
  int version;                                   /* MOTIRIS_PLUGIN_API_VERSION */
  void (*register_tool)(MotirisAgent *a, const MotirisTool *t);
  void (*unregister_tool)(MotirisAgent *a, const char *name);
  /* observe tool invocations (appends; coexists with repl/other hooks) */
  void (*add_tool_hook)(MotirisAgent *a, MotirisToolHook cb, void *ud);
} MotirisPluginApi;

#define MOTIRIS_PLUGIN_API_VERSION 1

/* A plugin shared object exports:
 *   int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api);
 * Returns 0 on success. Called at load time. */
typedef int (*motiris_plugin_init_fn)(MotirisAgent *a, const MotirisPluginApi *api);

int motiris_load_plugins(MotirisAgent *a, const char *dir);  /* 0 on success */
void motiris_set_transport(MotirisAgent *a, const char *name);  /* "curl"|"echo" */
void motiris_set_max_steps(MotirisAgent *a, int n);
void motiris_set_goal_check(MotirisAgent *a, int on);   /* confirm goal met */
void motiris_set_plugins_enabled(MotirisAgent *a, int on);
void motiris_set_tools_enabled(MotirisAgent *a, int on);
void motiris_set_plugin_dir(MotirisAgent *a, const char *dir);
int  motiris_plugins_enabled(MotirisAgent *a);
const char *motiris_plugin_dir(MotirisAgent *a);
void motiris_set_verbose(MotirisAgent *a, int on);
void motiris_set_stream(MotirisAgent *a, int on);      /* SSE streaming */
void motiris_set_stream_cb(MotirisAgent *a, void (*cb)(const char *, void *),
                           void *ud);   /* per-token sink (default stdout) */
/* Optional cooperative cancellation for provider requests and agent steps.
 * The callback runs in the calling thread and must be async-signal-safe when
 * it reads a flag set by a host signal handler. */
typedef int (MotirisCancelCallback)(void *ud);
void motiris_set_cancel_cb(MotirisAgent *a, MotirisCancelCallback *cb, void *ud);
enum MotirisRunResult {
  MOTIRIS_RUN_OK = 0,
  MOTIRIS_RUN_ERROR = 1,
  MOTIRIS_RUN_CANCELLED = 2,
};
int  motiris_run(MotirisAgent *a);   /* MotirisRunResult */
const char *motiris_last_error(MotirisAgent *a);

void motiris_set_tool_hook(MotirisAgent *a, MotirisToolHook cb, void *ud);
/* append another observer without replacing existing ones */
void motiris_add_tool_hook(MotirisAgent *a, MotirisToolHook cb, void *ud);

/* cumulative token counters across all turns of this agent */
long motiris_tokens_in(MotirisAgent *a);
long motiris_tokens_out(MotirisAgent *a);

/* built-in tools */
void motiris_register_core_tools(MotirisAgent *a);
void motiris_register_file_tools(MotirisAgent *a);  /* read/write/patch/search */
void motiris_register_web_tools(MotirisAgent *a);   /* web_fetch/search */
void motiris_register_memory_tools(MotirisAgent *a); /* memory get/set/list */
void motiris_register_skill_tools(MotirisAgent *a, const char *dir);
int  motiris_skill_count(void);                      /* indexed skills */
const char *motiris_skill_name(int i);               /* or NULL */
const char *motiris_skill_desc(int i);               /* or NULL */
void motiris_register_subagent_tools(MotirisAgent *a);  /* subagent */
void motiris_register_browser_tools(MotirisAgent *a);  /* browser_fetch */
int  motiris_tools_enabled(MotirisAgent *a);

/* Optional host-backed vision tool (Makefile VISION_TOOLS=1). */
#ifdef MOTIRIS_VISION_TOOLS
void motiris_register_vision_tools(MotirisAgent *a);
const char *motiris_base_url(MotirisAgent *a);
const char *motiris_api_key(MotirisAgent *a);
const char *motiris_transport_name(MotirisAgent *a);
#endif

/* ---- centralized config: $HOME/.motiris/{env,config.json} ---- */
void motiris_load_env(void);                  /* call before new() */
void motiris_apply_config(MotirisAgent *a);   /* defaults, CLI wins */
int  motiris_init_config(void);               /* write templates */
/* Atomically update one whitelisted scalar config.json key. Prints only the
 * key/path on success, never the value. Returns 0 on success, nonzero on error. */
int  motiris_set_config(const char *key, const char *value);

/* ---- shell safety policy (issue #10) ---- */
void motiris_set_shell_policy(const char *allow_csv, const char *deny_csv);
/* returns NULL if allowed, else malloc'd refusal reason */
char *motiris_shell_policy_check(const char *command);

/* ---- gateway: embedded HTTP service, session-per-chat_id ---- */
int motiris_gateway_run(const char *listen_addr, const char *token,
                        int tools_on, int verbose);  /* blocks; 0 on clean exit */

/* ---- plugin tools: per-tool directory layout (schema files) ---- */
char *motiris_plugin_dir_default(void);  /* strdup'd; env > ~/.motiris/plugins > ./plugins */
char *motiris_tools_dir_default(void);   /* strdup'd; env > ~/.motiris/tools */
void  motiris_tool_scan(const char *plugin_dir);    /* index <dir>/<name>/<name>.json schemas */
const char *motiris_tool_schema(const char *name);  /* external schema or NULL */

/* ---- sessions: host trace listing + bounded full-turn checkpoints ---- */
char *motiris_state_dir(const char *sub);  /* mkdir -p, strdup'd; NULL no HOME */
int  motiris_session_list(const char *sub, const char *term);   /* prints */
/* Portable memory-only snapshots: export returns a malloc'd, NUL-terminated
 * buffer owned by the caller; length excludes the NUL. Import requires that
 * snapshot[length] is readable and NUL, and applies the same 1 MiB / 256
 * message / 256 KiB record limits and full-turn validation as file restore.
 * No storage is retained by the core; the caller supplies any persistence. */
int  motiris_session_export(MotirisAgent *agent, const char *session_id,
                            char **snapshot, size_t *length,
                            const char **error);
int  motiris_session_import(MotirisAgent *agent, const char *snapshot,
                            size_t length, const char *expected_session_id,
                            size_t *loaded_messages, const char **error);
/* Legacy human-trace append only; never accepted as checkpoint restore input. */
int  motiris_session_append(const char *sub, const char *id, char tag,
                            const char *line);  /* 0 on success */
/* Versioned JSON snapshots: 1 MiB / 256 messages / 256 KiB per record.
 * Save atomically replaces path; load requires an empty agent context and
 * validates roles, tool pairing, ownership and integrity before restoring.
 * session_id may be NULL/empty for standalone CLI files. PERSISTENCE=0
 * replaces these host-file entry points with fixed-error, no-I/O stubs;
 * the portable memory export/import API above remains in the core library. */
int  motiris_session_save(const char *path, MotirisAgent *agent,
                          const char *session_id, const char **error);
int  motiris_session_checkpoint(const char *sub, const char *id,
                                MotirisAgent *agent, const char *session_id,
                                const char **error);
int  motiris_session_load(MotirisAgent *agent, const char *path,
                          const char *expected_session_id,
                          size_t *loaded_messages, const char **error);

/* ---- cron: scheduled agent runs ---- */
int motiris_cron_run(const char *jobs_file, int once, int verbose);

/* ---- MCP: register remote tools from an external stdio server ---- */
int motiris_register_mcp_server(MotirisAgent *a, const char *name,
                                const char *cmd, const char *const *args);

#endif /* MOTIRIS_H */