/* config.c - centralized configuration under $HOME/.motiris/
 *
 *   env          KEY=VALUE lines. MOTIRIS_* variables here act as
 *                defaults: a variable already exported in the calling
 *                environment wins. Parsed (never sourced) - plain text.
 *   config.json  optional JSON: model, base_url, api_key_env,
 *                max_steps, max_retries, retry_base_seconds, transport, stream,
 *                approval, reasoning_effort, think_mode, max_tokens,
 *                show_thinking, system, plugin_dir,
 *                tools, plugins.
 *
 * Precedence: CLI flags > config.json > env file > built-in defaults.
 * Missing files are fine; run `motiris --init` to generate templates.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MOTIRIS_NO_FILESYSTEM
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#endif

#ifndef MOTIRIS_NO_FILESYSTEM
#define CONFIG_MAX_BYTES (1 << 20)

static char *config_dir(void) {
  const char *h = getenv("MOTIRIS_HOME");
  if (h && *h) return strdup(h);
  const char *home = getenv("HOME");
  if (!home) home = ".";
  size_t n = strlen(home) + 16;
  char *p = malloc(n);
  snprintf(p, n, "%s/.motiris", home);
  return p;
}

/* Only keys whose provenance changes config.json precedence need tracking. */
static char file_envs[3][32];
static int n_file_envs;

static int precedence_key(const char *key) {
  return !strcmp(key, "MOTIRIS_MODEL") ||
         !strcmp(key, "MOTIRIS_BASE_URL") ||
         !strcmp(key, "MOTIRIS_GOAL_CHECK");
}

static void setenv_default(const char *key, const char *val) {
  if (val && *val && !getenv(key)) {
    setenv(key, val, 0);
    if (precedence_key(key) && n_file_envs < 3)
      snprintf(file_envs[n_file_envs++], 32, "%.31s", key);
  }
}

/* 1 = shell-exported (wins over config.json); 0 = unset or from env file */
static int env_overrides(const char *key) {
  if (!getenv(key)) return 0;
  for (int i = 0; i < n_file_envs; i++)
    if (!strcmp(file_envs[i], key)) return 0;
  return 1;
}

/* parse KEY=VALUE lines; apply MOTIRIS_* vars as defaults.
 * Expansion is bounded and copies replacements before moving overlapping
 * inline defaults. Invalid/recursive entries are skipped with no value dump. */
#define ENV_EXPANSION_MAX 64
static int expand_var(char *v, size_t cap) {
  for (int expansions = 0; expansions < ENV_EXPANSION_MAX; expansions++) {
    char *dollar = strchr(v, '$');
    if (!dollar || dollar[1] != '{') return 0;
    char *end = strchr(dollar, '}');
    if (!end) return -1;
    *end = '\0';
    char *name = dollar + 2;
    const char *dflt = NULL;
    char *dash = strstr(name, ":-");
    if (dash) { *dash = '\0'; dflt = dash + 2; }
    if (!*name) return -1;
    const char *val = getenv(name);
    const char *use = (val && *val) ? val : (dflt ? dflt : "");
    size_t used = strlen(use), tail = strlen(end + 1);
    if (used + tail + (size_t)(dollar - v) + 1 > cap) return -1;
    char *replacement = malloc(used + 1);
    if (!replacement) return -1;
    memcpy(replacement, use, used + 1);
    memmove(dollar + used, end + 1, tail + 1);
    memcpy(dollar, replacement, used);
    free(replacement);
  }
  return -1;
}

static void load_env_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    if (!strchr(line, '\n') && !feof(f)) {
      int c;
      while ((c = fgetc(f)) != '\n' && c != EOF) {}
      fprintf(stderr, "motiris: %s: oversized env line skipped\n", path);
      continue;
    }
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *k = line;
    while (*k == ' ' || *k == '\t') k++;
    char *ke = k + strlen(k);
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r'))
      *--ke = '\0';
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r')) v[--vl] = '\0';
    if (expand_var(v, sizeof line - (size_t)(v - line))) {
      fprintf(stderr, "motiris: %s: invalid or recursive expansion skipped\n",
              path);
      continue;
    }
    if (!strncmp(k, "MOTIRIS_", 8) && *v) setenv_default(k, v);
  }
  fclose(f);
}

void motiris_load_env(void) {
  char *d = config_dir();
  size_t n = strlen(d) + 16;
  char *p = malloc(n);
  snprintf(p, n, "%s/env", d);
  load_env_file(p);
  free(p);
  free(d);
}

static const char *jstr(const cJSON *o, const char *key) {
  const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jbool(const cJSON *o, const char *key, int dflt) {
  const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
  return v ? cJSON_IsTrue(v) : dflt;
}

static int jint(const cJSON *o, const char *key, int dflt, int minimum,
                int maximum) {
  const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
  if (!v) return dflt;
  double number = cJSON_GetNumberValue(v);
  if (!cJSON_IsNumber(v) || number < minimum || number > maximum ||
      (double)(int)number != number) {
    fprintf(stderr, "motiris: %s must be an integer in %d..%d; using %d\n",
            key, minimum, maximum, dflt);
    return dflt;
  }
  return (int)number;
}

#define SYSTEM_DOC_MAX_BYTES (256 * 1024)

static void load_system_doc(MotirisAgent *a, const char *dir,
                            const char *name) {
  size_t cap = strlen(dir) + strlen(name) + 2;
  char *path = malloc(cap);
  if (!path) return;
  snprintf(path, cap, "%s/%s", dir, name);
  FILE *f = fopen(path, "r");
  if (!f) { free(path); return; }
  struct stat st;
  if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      st.st_size > SYSTEM_DOC_MAX_BYTES) {
    fprintf(stderr, "motiris: %s: invalid or oversized system document skipped\n",
            path);
    fclose(f);
    free(path);
    return;
  }
  size_t size = (size_t)st.st_size;
  char *text = malloc(size + 1);
  if (text) {
    size_t got = fread(text, 1, size, f);
    if (got == size && !ferror(f) && !memchr(text, '\0', size)) {
      text[size] = '\0';
      motiris_append_system(a, text);
    } else {
      fprintf(stderr, "motiris: %s: unreadable system document skipped\n", path);
    }
    free(text);
  }
  fclose(f);
  free(path);
}

static void load_system_documents(MotirisAgent *a, const char *dir) {
  /* Persona first, operational instructions second. CLI --system applied
   * after config remains an explicit full override. */
  load_system_doc(a, dir, "SOUL.md");
  load_system_doc(a, dir, "AGENTS.md");
}

/* Apply config defaults without overriding a pre-disabled tools policy.
 * CLI/embedders can disable tools before this call to suppress MCP startup. */
void motiris_apply_config(MotirisAgent *a) {
  int tools_forced_off = !motiris_tools_enabled(a);
  char *d = config_dir();
  size_t n = strlen(d) + 32;
  char *p = malloc(n);
  snprintf(p, n, "%s/config.json", d);
  FILE *f = fopen(p, "r");
  if (!f) { load_system_documents(a, d); free(p); free(d); return; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > CONFIG_MAX_BYTES) {
    fclose(f);
    load_system_documents(a, d);
    free(p);
    free(d);
    return;
  }
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    load_system_documents(a, d);
    free(p);
    free(d);
    return;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  buf[got] = '\0';
  fclose(f);

  cJSON *j = got == (size_t)sz ? cJSON_Parse(buf) : NULL;
  free(buf);
  if (!j) { load_system_documents(a, d); free(p); free(d); return; }

  const char *s;
  if ((s = jstr(j, "model")) && !env_overrides("MOTIRIS_MODEL"))
    motiris_set_model(a, s);
  if ((s = jstr(j, "base_url")) && !env_overrides("MOTIRIS_BASE_URL"))
    motiris_set_base_url(a, s);
  if ((s = jstr(j, "api_key_env"))) {
    const char *k = getenv(s);
    if (k) motiris_set_api_key(a, k);
  }
  if ((s = jstr(j, "transport"))) motiris_set_transport(a, s);
  if ((s = jstr(j, "plugin_dir"))) motiris_set_plugin_dir(a, s);
  if ((s = jstr(j, "system"))) motiris_set_system(a, s);
  cJSON *steps = cJSON_GetObjectItemCaseSensitive(j, "max_steps");
  if (cJSON_IsNumber(steps)) {
    double value = cJSON_GetNumberValue(steps);
    if (value >= 0 && value <= INT_MAX && (double)(int)value == value)
      motiris_set_max_steps(a, (int)value);
    else
      fprintf(stderr, "motiris: invalid max_steps; using existing value\n");
  }
  if (cJSON_GetObjectItemCaseSensitive(j, "max_retries")) {
    int value = jint(j, "max_retries", motiris_max_retries(a), 0, 8);
    if (motiris_set_max_retries(a, value))
      fprintf(stderr, "motiris: invalid max_retries; using existing value\n");
  }
  if (cJSON_GetObjectItemCaseSensitive(j, "retry_base_seconds")) {
    int value = jint(j, "retry_base_seconds",
                     motiris_retry_base_seconds(a), 0, 60);
    if (motiris_set_retry_base_seconds(a, value))
      fprintf(stderr, "motiris: invalid retry_base_seconds; using existing value\n");
  }
  if (cJSON_GetObjectItemCaseSensitive(j, "stream"))
    motiris_set_stream(a, jbool(j, "stream", 1));
  if ((s = jstr(j, "approval"))) {
    if (!strcmp(s, "off")) motiris_set_approval_required(a, 0);
    else if (!strcmp(s, "ask")) motiris_set_approval_required(a, 1);
    else fprintf(stderr, "motiris: approval must be off or ask\n");
  }
  motiris_set_workspace_guard(a, jbool(j, "workspace_guard", 0));
  if ((s = jstr(j, "reasoning_effort")) &&
      motiris_set_reasoning_effort(a, s))
    fprintf(stderr, "motiris: reasoning_effort must be off, low, medium or high\n");
  if ((s = jstr(j, "think_mode")) && motiris_set_think_mode(a, s))
    fprintf(stderr, "motiris: think_mode must be provider, on or off\n");
  if (cJSON_GetObjectItemCaseSensitive(j, "max_tokens"))
    motiris_set_max_tokens(a, jint(j, "max_tokens", motiris_max_tokens(a),
                                  0, INT_MAX));
  if (cJSON_GetObjectItemCaseSensitive(j, "show_thinking"))
    motiris_set_thinking_visible(a, jbool(j, "show_thinking", 0));
  int compact = jbool(j, "context_compaction", 0);
  int compact_threshold = jint(j, "compaction_threshold", 32, 4, 256);
  int compact_keep = jint(j, "compaction_keep", 8, 1, 128);
  int compact_chars = jint(j, "compaction_max_chars", 8192, 256, 65536);
  if (motiris_set_compaction(a, compact, (size_t)compact_threshold,
                             (size_t)compact_keep, (size_t)compact_chars))
    fprintf(stderr, "motiris: compaction_keep must be smaller than compaction_threshold\n");
  if (!tools_forced_off && cJSON_GetObjectItemCaseSensitive(j, "tools"))
    motiris_set_tools_enabled(a, jbool(j, "tools", 1));
  if (cJSON_GetObjectItemCaseSensitive(j, "plugins"))
    motiris_set_plugins_enabled(a, jbool(j, "plugins", 1));
  if (cJSON_GetObjectItemCaseSensitive(j, "goal_check") && !env_overrides("MOTIRIS_GOAL_CHECK"))
    motiris_set_goal_check(a, jbool(j, "goal_check", 0));

  /* shell safety policy: "shell_allow": "git,ls", "shell_deny": "rm,sudo" */
  motiris_set_shell_policy(jstr(j, "shell_allow"), jstr(j, "shell_deny"));

  /* MCP servers: [{name, cmd, args: []}] -> register remote tools */
  cJSON *mcps = cJSON_GetObjectItemCaseSensitive(j, "mcp_servers");
  if (motiris_tools_enabled(a) && cJSON_IsArray(mcps)) {
    cJSON *srv;
    cJSON_ArrayForEach(srv, mcps) {
      const char *nm = jstr(srv, "name");
      const char *cmd = jstr(srv, "cmd");
      if (!nm || !cmd) continue;
      const char *args[16];
      int na = 0;
      cJSON *ar = cJSON_GetObjectItemCaseSensitive(srv, "args");
      if (cJSON_IsArray(ar)) {
        cJSON *ai;
        cJSON_ArrayForEach(ai, ar)
          if (na < 15 && cJSON_IsString(ai)) args[na++] = ai->valuestring;
      }
      args[na] = NULL;
      motiris_register_mcp_server(a, nm, cmd, args);
    }
  }

  /* provider fallback list: [{name, model, base_url, api_key_env}] */
  cJSON *provs = cJSON_GetObjectItemCaseSensitive(j, "providers");
  if (cJSON_IsArray(provs)) {
    for (cJSON *p = provs->child; p; p = p->next) {
      if (!cJSON_IsObject(p)) continue;
      const char *pm = jstr(p, "model");
      const char *pu = jstr(p, "base_url");
      if (!pm || !pu) continue;
      const char *keyenv = jstr(p, "api_key_env");
      MotirisProvider mp = {
        .name = jstr(p, "name"),
        .model = pm,
        .base_url = pu,
        .api_key = keyenv ? getenv(keyenv) : NULL,
      };
      motiris_add_provider(a, &mp);
    }
  }
  cJSON_Delete(j);
  load_system_documents(a, d);
  free(p);
  free(d);
}

/* Conservative, all-or-nothing template initialization (issue #74). */
static int init_write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (!n) errno = EIO;
    return -1;
  }
  return 0;
}

static int init_write_new(const char *path, const char *content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  size_t len = strlen(content);
  if (init_write_all(fd, content, len) || fsync(fd)) {
    int saved = errno;
    close(fd);
    unlink(path);
    errno = saved;
    return -1;
  }
  if (close(fd)) {
    int saved = errno;
    unlink(path);
    errno = saved;
    return -1;
  }
  return 0;
}

int motiris_init_config(void) {
  static const char *const names[] = {
    "env", "config.json", "AGENTS.md", "SOUL.md",
    "skills/motiris-config/SKILL.md",
    "skills/motiris-session/SKILL.md",
    "skills/motiris-context-compaction/SKILL.md"
  };
  static const char config_template[] =
    "{\n"
    "  \"model\": \"glm-5.1\",\n"
    "  \"base_url\": \"https://token.moi.matrixorigin.cn/v1/chat/completions\",\n"
    "  \"api_key_env\": \"MOI_TAAS_API_KEY\",\n"
    "  \"max_steps\": 0,\n"
    "  \"max_retries\": 0,\n"
    "  \"retry_base_seconds\": 1,\n"
    "  \"transport\": \"auto\",\n"
    "  \"stream\": true,\n"
    "  \"approval\": \"off\",\n"
    "  \"workspace_guard\": false,\n"
    "  \"reasoning_effort\": \"medium\",\n"
    "  \"show_thinking\": false,\n"
    "  \"context_compaction\": false,\n"
    "  \"compaction_threshold\": 32,\n"
    "  \"compaction_keep\": 8,\n"
    "  \"compaction_max_chars\": 8192,\n"
    "  \"tools\": true,\n"
    "  \"plugins\": true,\n"
    "  \"system\": \"You are iris, a concise helpful assistant.\"\n"
    "}\n";
  static const char agents_template[] =
    "# AGENTS.md\n\n"
    "- Inspect before changing; keep changes bounded and verify the result.\n"
    "- Use the motiris-config skill for runtime and JSON configuration.\n"
    "- approval and workspace_guard are off by default; enable guards when boundaries are wanted.\n"
    "- Never overwrite existing configuration or credentials during initialization.\n"
    "- Whenever a feature changes, update its relevant default installed SKILL.md in the same change.\n";
  static const char soul_template[] =
    "# SOUL.md\n\n"
    "You are iris: direct, helpful and concise. State uncertainty plainly,\n"
    "prefer verifiable facts, and keep the user's workspace and choices respected.\n";
  static const char skill_template[] =
    "---\n"
    "name: motiris-config\n"
    "description: Configure motiris runtime, streaming, reasoning and tool approval.\n"
    "---\n"
    "# Motiris configuration\n\n"
    "Use `motiris set config KEY VALUE` or `motiris --config KEY VALUE` for JSON.\n"
    "Keys: stream (boolean), max_steps (0 unlimited or positive), max_retries (0..8),\n"
    "retry_base_seconds (0..60), approval (off|ask),\n"
    "reasoning_effort (off|low|medium|high), think_mode (provider|on|off),\n"
    "max_tokens (0 provider default or positive), show_thinking (boolean),\n"
    "workspace_guard (boolean; off permits any OS-accessible path).\n"
    "In the REPL use /config, /model, /approval, /reasoning and /thinking.\n"
    "Session card changes are immediate; only the explicit save action persists\n"
    "them. /model edits model_id/endpoint/parameters and performs model discovery\n"
    "only when Refresh models is selected. Enter on model_id expands fetched\n"
    "choices; e edits manually. Closing after changes asks to save, keep\n"
    "session-only values or return, and failed saves stay open for retry.\n"
    "API keys stay hidden and are saved\n"
    "only in the mode-0600 env file, referenced by api_key_env.\n"
    "Compaction keys: context_compaction, compaction_threshold, compaction_keep,\n"
    "compaction_max_chars. Enable compaction only when its optional plugin exists.\n"
    "Skill discovery scans user < workspace < explicit directories. Only title,\n"
    "description and path enter the system index; use read_file on an indexed path\n"
    "to load the full body on demand. Do not print credentials. Keep existing\n"
    "env/config/Markdown files unchanged unless explicitly asked to edit them.\n";
  static const char session_skill_template[] =
    "---\nname: motiris-session\n"
    "description: Inspect, save, restore, and reset automatic UUID session traces.\n---\n"
    "# Motiris sessions\n\n"
    "Automatic state lives under MOTIRIS_HOME (default $HOME/.motiris):\n"
    "session/session.json is the index and session/session-<uuid>.json is each trace.\n"
    "REPL turns save automatically. Use /save to save now, /load for an indexed\n"
    "restore into an empty context, and /new to start a fresh UUID without carrying\n"
    "approval grants. Gateway sessions restore and reset by chat_id. Use the session\n"
    "tools for legacy checkpoint logs. During generation Enter queues up to 32 lines\n"
    "(64 KiB total); Esc/Ctrl+C cancels without clearing the draft or queue. Queued\n"
    "text is traced only when dispatched; unsubmitted drafts are never traced.\n"
    "Never put reasoning side-channel text in traces.\n";
  static const char compaction_skill_template[] =
    "---\nname: motiris-context-compaction\n"
    "description: Configure optional deterministic bounded context compaction.\n---\n"
    "# Context compaction\n\n"
    "Keep context_compaction false unless the optional plugin is installed.\n"
    "Set compaction_threshold, compaction_keep and compaction_max_chars in config.json\n"
    "or with --config. The plugin summarizes old complete turns only and preserves\n"
    "the active user turn and tool-call/result pairs. Missing plugin or compaction\n"
    "failure stops the run without silently discarding context. Verify with\n"
    "tests/context_compaction.py. Update this Skill with every behavior change.\n";

  char *d = config_dir();
  if (!d) return 1;
  if (mkdir(d, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "motiris: cannot create %s\n", d);
    free(d);
    return 1;
  }
  size_t n = strlen(d) + 64;
  char *path = malloc(n);
  size_t env_cap = strlen(d) + 1024;
  char *env_text = malloc(env_cap);
  if (!path || !env_text) {
    free(path); free(env_text); free(d);
    return 1;
  }
  snprintf(env_text, env_cap,
           "# motiris env defaults (MOTIRIS_* here lose to your shell)\n"
           "MOTIRIS_API_KEY=${MOI_TAAS_API_KEY:-}\n"
           "MOTIRIS_MODEL=glm-5.1\n"
           "MOTIRIS_BASE_URL=https://token.moi.matrixorigin.cn/v1/chat/completions\n"
           "MOTIRIS_PLUGIN_DIR=%s/plugins\n", d);
  const char *contents[] = {
    env_text, config_template, agents_template, soul_template, skill_template,
    session_skill_template, compaction_skill_template
  };

  /* Any existing target aborts the whole operation before the first write. */
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, n, "%s/%s", d, names[i]);
    struct stat st;
    if (!lstat(path, &st)) {
      fprintf(stderr, "motiris: %s exists; existing files kept unchanged\n", path);
      free(path); free(env_text); free(d);
      return 1;
    }
    if (errno != ENOENT) {
      fprintf(stderr, "motiris: cannot inspect %s\n", path);
      free(path); free(env_text); free(d);
      return 1;
    }
  }
  int dirs_ok = 1;
  snprintf(path, n, "%s/skills", d);
  if (mkdir(path, 0755) && errno != EEXIST) dirs_ok = 0;
  static const char *const skill_dirs[] = {
    "motiris-config", "motiris-session", "motiris-context-compaction"
  };
  for (size_t i = 0; dirs_ok && i < sizeof skill_dirs / sizeof skill_dirs[0]; i++) {
    snprintf(path, n, "%s/skills/%s", d, skill_dirs[i]);
    if (mkdir(path, 0755) && errno != EEXIST) dirs_ok = 0;
  }
  if (!dirs_ok) {
    fprintf(stderr, "motiris: cannot create skill directories under %s\n", d);
    free(path);
    free(env_text);
    free(d);
    return 1;
  }

  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, n, "%s/%s", d, names[i]);
    if (init_write_new(path, contents[i])) {
      fprintf(stderr, "motiris: cannot initialize %s: %s\n", path,
              strerror(errno));
      for (size_t j = 0; j < i; j++) {
        snprintf(path, n, "%s/%s", d, names[j]);
        unlink(path);
      }
      free(path); free(env_text); free(d);
      return 1;
    }
  }
  printf("motiris: initialized %s (max_steps=0, SSE on, approval off)\n", d);
  free(path);
  free(env_text);
  free(d);
  return 0;
}

/* ---------------- atomic scalar config editor ---------------- */
enum ConfigValueType { CONFIG_STRING, CONFIG_BOOL, CONFIG_INT };
typedef struct {
  const char *name;
  int type;
  int nonempty;
} ConfigKey;

static const ConfigKey CONFIG_KEYS[] = {
  { "model", CONFIG_STRING, 1 },
  { "base_url", CONFIG_STRING, 1 },
  { "api_key_env", CONFIG_STRING, 1 },
  { "transport", CONFIG_STRING, 1 },
  { "system", CONFIG_STRING, 0 },
  { "plugin_dir", CONFIG_STRING, 0 },
  { "shell_allow", CONFIG_STRING, 0 },
  { "shell_deny", CONFIG_STRING, 0 },
  { "stream", CONFIG_BOOL, 0 },
  { "show_thinking", CONFIG_BOOL, 0 },
  { "workspace_guard", CONFIG_BOOL, 0 },
  { "context_compaction", CONFIG_BOOL, 0 },
  { "compaction_threshold", CONFIG_INT, 0 },
  { "compaction_keep", CONFIG_INT, 0 },
  { "compaction_max_chars", CONFIG_INT, 0 },
  { "approval", CONFIG_STRING, 1 },
  { "reasoning_effort", CONFIG_STRING, 1 },
  { "think_mode", CONFIG_STRING, 1 },
  { "max_tokens", CONFIG_INT, 0 },
  { "tools", CONFIG_BOOL, 0 },
  { "plugins", CONFIG_BOOL, 0 },
  { "goal_check", CONFIG_BOOL, 0 },
  { "max_steps", CONFIG_INT, 0 },
  { "max_retries", CONFIG_INT, 0 },
  { "retry_base_seconds", CONFIG_INT, 0 },
};

static int config_error(const char *path, const char *reason) {
  fprintf(stderr, "motiris: %s: %s\n", path, reason);
  return 1;
}

static int config_sys_error(const char *operation, const char *path) {
  fprintf(stderr, "motiris: %s %s: %s\n", operation, path, strerror(errno));
  return 1;
}

static int config_write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (!n) errno = EIO;
    return -1;
  }
  return 0;
}

/* ---------------- atomic env-file editor ---------------- */
#define ENV_FILE_MAX_BYTES (1 << 20)
#define ENV_LINE_MAX_BYTES 4096
#define ENV_API_KEY_MAX_BYTES 489

static int env_edit_error(const char *reason) {
  fprintf(stderr, "motiris: cannot update env: %s\n", reason);
  return -1;
}

static int env_key_safe(const char *key) {
  if (!key || strncmp(key, "MOTIRIS_", 8) || !key[8]) return 0;
  for (const unsigned char *p = (const unsigned char *)key + 8; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}

static int env_value_safe(const char *key, const char *value) {
  if (!value || !*value || strstr(value, "${")) return 0;
  size_t length = strlen(value);
  if (strchr(" \t\r\n\v\f", value[0]) ||
      strchr(" \t\r\n\v\f", value[length - 1])) return 0;
  for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    if (*p < 0x20 || *p == 0x7f) return 0;
  if (!strcmp(key, "MOTIRIS_API_KEY") && length > ENV_API_KEY_MAX_BYTES)
    return 0;
  /* load_env_file uses a 4096-byte fgets buffer including NUL. */
  return strlen(key) + length + 2 < ENV_LINE_MAX_BYTES;
}

static int env_line_matches(const char *line, size_t length, const char *key) {
  size_t start = 0;
  while (start < length && (line[start] == ' ' || line[start] == '\t')) start++;
  size_t end = start;
  while (end < length && line[end] != '=' && line[end] != '\n' &&
         line[end] != '\r') end++;
  if (end >= length || line[end] != '=') return 0;
  size_t trimmed = end;
  while (trimmed > start &&
         (line[trimmed - 1] == ' ' || line[trimmed - 1] == '\t')) trimmed--;
  size_t key_length = strlen(key);
  return trimmed - start == key_length &&
         !memcmp(line + start, key, key_length);
}

int motiris_set_env(const char *key, const char *value) {
  if (!env_key_safe(key)) return env_edit_error("unsafe key");
  if (!env_value_safe(key, value)) return env_edit_error("unsafe value");
  char *dir = config_dir();
  if (!dir) return env_edit_error("out of memory");
  size_t path_cap = strlen(dir) + 16;
  char *path = malloc(path_cap);
  if (!path) { free(dir); return env_edit_error("out of memory"); }
  snprintf(path, path_cap, "%s/env", dir);
  char *source = NULL, *updated = NULL, *temp = NULL;
  size_t size = 0, updated_length = 0;
  int fd = -1, temp_fd = -1, temp_created = 0, replaced = 0;

  fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > ENV_FILE_MAX_BYTES) {
      env_edit_error("invalid or oversized existing file");
      goto fail;
    }
    size = (size_t)st.st_size;
    source = malloc(size + 1);
    if (!source) { env_edit_error("out of memory"); goto fail; }
    size_t off = 0;
    while (off < size) {
      ssize_t n = read(fd, source + off, size - off);
      if (n > 0) { off += (size_t)n; continue; }
      if (n < 0 && errno == EINTR) continue;
      env_edit_error(n < 0 ? "cannot read existing file" :
                           "existing file changed while reading");
      goto fail;
    }
    char extra;
    ssize_t n;
    do n = read(fd, &extra, 1); while (n < 0 && errno == EINTR);
    if (n != 0) {
      env_edit_error(n < 0 ? "cannot read existing file" :
                           "existing file changed while reading");
      goto fail;
    }
    source[size] = '\0';
    close(fd);
    fd = -1;
  } else if (errno != ENOENT) {
    env_edit_error("cannot open existing file");
    goto fail;
  } else {
    source = malloc(1);
    if (!source) { env_edit_error("out of memory"); goto fail; }
    source[0] = '\0';
  }

  size_t key_length = strlen(key), value_length = strlen(value);
  size_t updated_cap = size + key_length + value_length + 3;
  updated = malloc(updated_cap);
  if (!updated) { env_edit_error("out of memory"); goto fail; }
  size_t off = 0;
  while (off < size) {
    size_t end = off;
    while (end < size && source[end] != '\n') end++;
    size_t next = end < size ? end + 1 : end;
    if (env_line_matches(source + off, end - off, key)) {
      if (!replaced) {
        memcpy(updated + updated_length, key, key_length);
        updated_length += key_length;
        updated[updated_length++] = '=';
        memcpy(updated + updated_length, value, value_length);
        updated_length += value_length;
        if (next > end) updated[updated_length++] = '\n';
        replaced = 1;
      }
    } else {
      memcpy(updated + updated_length, source + off, next - off);
      updated_length += next - off;
    }
    off = next;
  }
  if (!replaced) {
    if (updated_length && updated[updated_length - 1] != '\n')
      updated[updated_length++] = '\n';
    memcpy(updated + updated_length, key, key_length);
    updated_length += key_length;
    updated[updated_length++] = '=';
    memcpy(updated + updated_length, value, value_length);
    updated_length += value_length;
    updated[updated_length++] = '\n';
  }

  if (mkdir(dir, 0755) && errno != EEXIST) {
    env_edit_error("cannot create config directory");
    goto fail;
  }
  temp = malloc(strlen(path) + 16);
  if (!temp) { env_edit_error("out of memory"); goto fail; }
  sprintf(temp, "%s.tmp.XXXXXX", path);
  temp_fd = mkstemp(temp);
  if (temp_fd < 0) { env_edit_error("cannot create temporary file"); goto fail; }
  temp_created = 1;
  if (config_write_all(temp_fd, updated, updated_length) || fsync(temp_fd)) {
    env_edit_error("cannot write temporary file");
    goto fail;
  }
  if (close(temp_fd)) {
    temp_fd = -1;
    env_edit_error("cannot close temporary file");
    goto fail;
  }
  temp_fd = -1;
  if (rename(temp, path)) {
    env_edit_error("cannot replace existing file");
    goto fail;
  }
  temp_created = 0;
  free(temp);
  free(updated);
  free(source);
  free(path);
  free(dir);
  return 0;

fail:
  if (fd >= 0) close(fd);
  if (temp_fd >= 0) close(temp_fd);
  if (temp_created) unlink(temp);
  free(temp);
  free(updated);
  free(source);
  free(path);
  free(dir);
  return -1;
}

int motiris_set_config(const char *key, const char *value) {
  const ConfigKey *spec = NULL;
  if (key && value) {
    for (size_t i = 0; i < sizeof CONFIG_KEYS / sizeof CONFIG_KEYS[0]; i++)
      if (!strcmp(key, CONFIG_KEYS[i].name)) { spec = &CONFIG_KEYS[i]; break; }
  }
  if (!spec) {
    fprintf(stderr, "motiris: unsupported config key (use model, base_url, "
            "api_key_env, transport, system, plugin_dir, shell_allow, "
            "shell_deny, stream, show_thinking, workspace_guard, approval, reasoning_effort, "
            "context_compaction, compaction_threshold, compaction_keep, "
            "compaction_max_chars, think_mode, max_tokens, tools, plugins, "
            "goal_check, max_steps, max_retries or retry_base_seconds)\n");
    return 1;
  }
  if (spec->nonempty && !*value) {
    fprintf(stderr, "motiris: %s cannot be empty\n", key);
    return 1;
  }

  int bool_value = 0;
  long int_value = 0;
  if (spec->type == CONFIG_BOOL) {
    if (!strcmp(value, "true")) bool_value = 1;
    else if (strcmp(value, "false")) {
      fprintf(stderr, "motiris: %s must be true or false\n", key);
      return 1;
    }
  } else if (spec->type == CONFIG_INT) {
    if (*value < '0' || *value > '9') {
      fprintf(stderr, "motiris: %s must be an integer in 0..%d\n", key, INT_MAX);
      return 1;
    }
    char *end = NULL;
    errno = 0;
    int_value = strtol(value, &end, 10);
    if (errno == ERANGE || !end || *end || int_value < 0 || int_value > INT_MAX) {
      fprintf(stderr, "motiris: %s must be an integer in 0..%d\n", key, INT_MAX);
      return 1;
    }
    if ((!strcmp(key, "max_retries") && int_value > 8) ||
        (!strcmp(key, "retry_base_seconds") && int_value > 60)) {
      fprintf(stderr, "motiris: %s must be an integer in 0..%d\n",
              key, !strcmp(key, "max_retries") ? 8 : 60);
      return 1;
    }
  } else if ((!strcmp(key, "compaction_threshold") && (int_value < 4 || int_value > 256)) ||
             (!strcmp(key, "compaction_keep") && (int_value < 1 || int_value > 128)) ||
             (!strcmp(key, "compaction_max_chars") &&
              (int_value < 256 || int_value > 65536))) {
    fprintf(stderr, "motiris: invalid %s limit\n", key);
    return 1;
  } else if (!strcmp(key, "transport") && strcmp(value, "auto") &&
             strcmp(value, "libcurl") && strcmp(value, "curl") &&
             strcmp(value, "echo")) {
    fprintf(stderr, "motiris: transport must be auto, libcurl, curl or echo\n");
    return 1;
  } else if (!strcmp(key, "approval") && strcmp(value, "off") &&
             strcmp(value, "ask")) {
    fprintf(stderr, "motiris: approval must be off or ask\n");
    return 1;
  } else if (!strcmp(key, "reasoning_effort") && strcmp(value, "off") &&
             strcmp(value, "low") && strcmp(value, "medium") &&
             strcmp(value, "high")) {
    fprintf(stderr, "motiris: reasoning_effort must be off, low, medium or high\n");
    return 1;
  } else if (!strcmp(key, "think_mode") && strcmp(value, "provider") &&
             strcmp(value, "on") && strcmp(value, "off")) {
    fprintf(stderr, "motiris: think_mode must be provider, on or off\n");
    return 1;
  }

  char *dir = config_dir();
  if (!dir) { fprintf(stderr, "motiris: out of memory\n"); return 1; }
  size_t path_cap = strlen(dir) + 32;
  char *path = malloc(path_cap);
  if (!path) { free(dir); fprintf(stderr, "motiris: out of memory\n"); return 1; }
  snprintf(path, path_cap, "%s/config.json", dir);

  cJSON *root = NULL, *node = NULL;
  char *printed = NULL, *temp = NULL;
  int fd = -1, temp_fd = -1, temp_created = 0;

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd >= 0) {
    struct stat st;
    if (fstat(fd, &st)) { config_sys_error("cannot stat", path); goto fail; }
    if (!S_ISREG(st.st_mode)) { config_error(path, "not a regular file"); goto fail; }
    if (st.st_size <= 0 || st.st_size > CONFIG_MAX_BYTES) {
      config_error(path, "empty or oversized config is not overwritten");
      goto fail;
    }
    size_t size = (size_t)st.st_size, off = 0;
    char *buf = malloc(size + 1);
    if (!buf) { config_error(path, "out of memory"); goto fail; }
    while (off < size) {
      ssize_t n = read(fd, buf + off, size - off);
      if (n > 0) { off += (size_t)n; continue; }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) config_sys_error("cannot read", path);
      else config_error(path, "config changed while reading");
      free(buf);
      goto fail;
    }
    char extra;
    ssize_t n;
    do n = read(fd, &extra, 1); while (n < 0 && errno == EINTR);
    if (n < 0) { config_sys_error("cannot read", path); free(buf); goto fail; }
    if (n > 0) { config_error(path, "config changed while reading"); free(buf); goto fail; }
    buf[size] = '\0';
    close(fd);
    fd = -1;
    root = cJSON_ParseWithOpts(buf, NULL, 1);
    free(buf);
    if (!root) { config_error(path, "invalid JSON; original preserved"); goto fail; }
    if (!cJSON_IsObject(root)) {
      config_error(path, "top-level JSON must be an object; original preserved");
      goto fail;
    }
  } else if (errno != ENOENT) {
    config_sys_error("cannot open", path);
    goto fail;
  }

  if (!root) root = cJSON_CreateObject();
  if (spec->type == CONFIG_BOOL) node = cJSON_CreateBool(bool_value);
  else if (spec->type == CONFIG_INT) node = cJSON_CreateNumber((double)int_value);
  else node = cJSON_CreateString(value);
  if (!root || !node) { config_error(path, "out of memory"); goto fail; }

  cJSON *old = cJSON_GetObjectItemCaseSensitive(root, key);
  int updated = old ? cJSON_ReplaceItemInObjectCaseSensitive(root, key, node)
                    : cJSON_AddItemToObject(root, key, node);
  if (!updated) { cJSON_Delete(node); node = NULL; config_error(path, "out of memory"); goto fail; }
  node = NULL;
  printed = cJSON_Print(root);
  if (!printed) { config_error(path, "cannot serialize config"); goto fail; }
  size_t printed_len = strlen(printed);
  if (printed_len + 1 > CONFIG_MAX_BYTES) {
    config_error(path, "serialized config exceeds 1 MiB");
    goto fail;
  }

  if (mkdir(dir, 0755) && errno != EEXIST) {
    config_sys_error("cannot create", dir);
    goto fail;
  }
  temp = malloc(strlen(path) + 16);
  if (!temp) { config_error(path, "out of memory"); goto fail; }
  sprintf(temp, "%s.tmp.XXXXXX", path);
  temp_fd = mkstemp(temp);  /* mode 0600, same filesystem for atomic rename */
  if (temp_fd < 0) { config_sys_error("cannot create", temp); goto fail; }
  temp_created = 1;
  if (config_write_all(temp_fd, printed, printed_len) ||
      config_write_all(temp_fd, "\n", 1) || fsync(temp_fd)) {
    config_sys_error("cannot write", temp);
    goto fail;
  }
  if (close(temp_fd)) {
    temp_fd = -1;
    config_sys_error("cannot close", temp);
    goto fail;
  }
  temp_fd = -1;
  if (rename(temp, path)) { config_sys_error("cannot replace", path); goto fail; }
  temp_created = 0;

  printf("motiris: set %s in %s\n", key, path);
  free(temp);
  free(printed);
  cJSON_Delete(root);
  free(path);
  free(dir);
  return 0;

fail:
  if (fd >= 0) close(fd);
  if (temp_fd >= 0) close(temp_fd);
  if (temp_created) unlink(temp);
  free(temp);
  free(printed);
  cJSON_Delete(node);
  cJSON_Delete(root);
  free(path);
  free(dir);
  return 1;
}

#else
void motiris_load_env(void) {}
void motiris_apply_config(MotirisAgent *a) { (void)a; }
int motiris_init_config(void) { return 1; }
int motiris_set_config(const char *key, const char *value) {
  (void)key; (void)value;
  return 1;
}
int motiris_set_env(const char *key, const char *value) {
  (void)key; (void)value;
  return -1;
}
#endif

/* ---------------- shell safety policy (issue #10) ---------------- */
static char shell_allow[16][128];
static int n_allow;
static char shell_deny[16][128];
static int n_deny;

static void split_csv(char list[][128], int *n, const char *csv) {
  *n = 0;
  if (!csv) return;
  const char *p = csv;
  while (*p && *n < 16) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    while (len && (p[len-1] == ' ' || p[len-1] == '\t')) len--;
    if (len > 0) {
      if (len >= 128) len = 127;
      memcpy(list[*n], p, len);
      list[*n][len] = '\0';
      (*n)++;
    }
    if (!comma) break;
    p = comma + 1;
  }
}

void motiris_set_shell_policy(const char *allow_csv, const char *deny_csv) {
  split_csv(shell_allow, &n_allow, allow_csv);
  split_csv(shell_deny, &n_deny, deny_csv);
}

char *motiris_shell_policy_check(const char *command) {
  if (!command) return NULL;
  for (int i = 0; i < n_deny; i++) {
    if (!strncmp(command, shell_deny[i], strlen(shell_deny[i]))) {
      size_t need = strlen(shell_deny[i]) + 48;
      char *msg = malloc(need);
      snprintf(msg, need,
               "blocked by shell policy (deny prefix %s)", shell_deny[i]);
      return msg;
    }
  }
  if (n_allow > 0) {
    for (int i = 0; i < n_allow; i++) {
      if (!strncmp(command, shell_allow[i], strlen(shell_allow[i])))
        return NULL;
    }
    char *msg = malloc(256);
    snprintf(msg, 256,
             "blocked by shell policy (not in allowlist, e.g. %s)",
             shell_allow[0]);
    return msg;
  }
  return NULL;
}