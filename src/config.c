/* config.c - centralized configuration under $HOME/.motiris/
 *
 *   env          KEY=VALUE lines. MOTIRIS_* variables here act as
 *                defaults: a variable already exported in the calling
 *                environment wins. Parsed (never sourced) - plain text.
 *   config.json  optional JSON: model, base_url, api_key_env,
 *                max_steps, transport, stream, system, plugin_dir,
 *                tools, plugins.
 *
 * Precedence: CLI flags > config.json > env file > built-in defaults.
 * Missing files are fine; run `motiris --init` to generate templates.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char file_envs[16][64];
static int n_file_envs;

static void setenv_default(const char *key, const char *val) {
  if (val && *val && !getenv(key)) {
    setenv(key, val, 0);
    if (n_file_envs < 16) snprintf(file_envs[n_file_envs++], 64, "%s", key);
  }
}

/* 1 = shell-exported (wins over config.json); 0 = unset or from env file */
static int env_overrides(const char *key) {
  if (!getenv(key)) return 0;
  for (int i = 0; i < n_file_envs; i++)
    if (!strcmp(file_envs[i], key)) return 0;
  return 1;
}

/* parse KEY=VALUE lines; apply MOTIRIS_* vars as defaults */
/* expand ${NAME} and ${NAME:-default} in values (no shell involved) */
static void expand_var(char *v, size_t cap) {
  char *dollar = strchr(v, '$');
  while (dollar && dollar[1] == '{') {
    char *end = strchr(dollar, '}');
    if (!end) break;
    *end = '\0';
    char *name = dollar + 2;
    const char *dflt = NULL;
    char *dash = strstr(name, ":-");
    if (dash) { *dash = '\0'; dflt = dash + 2; }
    const char *val = getenv(name);
    const char *use = (val && *val) ? val : (dflt ? dflt : "");
    size_t used = strlen(use), tail = strlen(end + 1);
    if (used + tail + (size_t)(dollar - v) + 1 > cap) break;
    memmove(dollar + used, end + 1, tail + 1);
    memcpy(dollar, use, used);
    dollar = strchr(v, '$');
  }
}

static void load_env_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
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
    expand_var(v, sizeof line - (size_t)(v - line));
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

/* apply ~/.motiris/config.json defaults onto an agent (CLI overrides later) */
void motiris_apply_config(MotirisAgent *a) {
  char *d = config_dir();
  size_t n = strlen(d) + 32;
  char *p = malloc(n);
  snprintf(p, n, "%s/config.json", d);
  FILE *f = fopen(p, "r");
  if (!f) { free(p); free(d); return; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1 << 20) { fclose(f); free(p); free(d); return; }
  char *buf = malloc((size_t)sz + 1);
  buf[fread(buf, 1, (size_t)sz, f)] = '\0';
  fclose(f);

  cJSON *j = cJSON_Parse(buf);
  free(buf);
  if (!j) { free(p); free(d); return; }

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
  if (cJSON_GetObjectItemCaseSensitive(j, "max_steps"))
    motiris_set_max_steps(a, (int)cJSON_GetNumberValue(
        cJSON_GetObjectItemCaseSensitive(j, "max_steps")));
  if (cJSON_GetObjectItemCaseSensitive(j, "stream"))
    motiris_set_stream(a, jbool(j, "stream", 0));
  if (cJSON_GetObjectItemCaseSensitive(j, "tools"))
    motiris_set_tools_enabled(a, jbool(j, "tools", 1));
  if (cJSON_GetObjectItemCaseSensitive(j, "plugins"))
    motiris_set_plugins_enabled(a, jbool(j, "plugins", 1));
  cJSON_Delete(j);
  free(p);
  free(d);
}

/* write the two template files if the config directory is missing */
int motiris_init_config(void) {
  char *d = config_dir();
  if (mkdir(d, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "motiris: cannot create %s\n", d);
    free(d);
    return 1;
  }
  size_t n = strlen(d) + 16;
  char *p = malloc(n);
  int rc = 0;

  snprintf(p, n, "%s/env", d);
  FILE *f = fopen(p, "w");
  const char *home = getenv("HOME");
  if (f) {
    fprintf(f, "# motiris env defaults (MOTIRIS_* here lose to your shell)\n"
               "MOTIRIS_API_KEY=${MOI_TAAS_API_KEY:-}\n"
               "MOTIRIS_MODEL=glm-5.1\n"
               "MOTIRIS_BASE_URL="
               "https://token.moi.matrixorigin.cn/v1/chat/completions\n"
               "MOTIRIS_PLUGIN_DIR=%s/.local/share/motiris/plugins\n",
               home ? home : "");
    fclose(f);
  } else rc = 1;

  snprintf(p, n, "%s/config.json", d);
  f = fopen(p, "w");
  if (f) {
    fprintf(f, "{\n"
               "  \"model\": \"glm-5.1\",\n"
               "  \"base_url\": \"https://token.moi.matrixorigin.cn/v1/chat/completions\",\n"
               "  \"api_key_env\": \"MOI_TAAS_API_KEY\",\n"
               "  \"max_steps\": 10,\n"
               "  \"transport\": \"auto\",\n"
               "  \"stream\": true,\n"
               "  \"tools\": true,\n"
               "  \"plugins\": true,\n"
               "  \"system\": \"You are iris, a concise helpful assistant.\"\n"
               "}\n");
    fclose(f);
  } else rc = 1;

  free(p);
  free(d);
  return rc;
}