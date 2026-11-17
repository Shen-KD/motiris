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
  if (sz <= 0 || sz > CONFIG_MAX_BYTES) { fclose(f); free(p); free(d); return; }
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
  if (cJSON_GetObjectItemCaseSensitive(j, "goal_check") && !env_overrides("MOTIRIS_GOAL_CHECK"))
    motiris_set_goal_check(a, jbool(j, "goal_check", 0));

  /* shell safety policy: "shell_allow": "git,ls", "shell_deny": "rm,sudo" */
  motiris_set_shell_policy(jstr(j, "shell_allow"), jstr(j, "shell_deny"));

  /* MCP servers: [{name, cmd, args: []}] -> register remote tools */
  cJSON *mcps = cJSON_GetObjectItemCaseSensitive(j, "mcp_servers");
  if (cJSON_IsArray(mcps)) {
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
               "MOTIRIS_PLUGIN_DIR=%s/.motiris/plugins\n",
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
  { "tools", CONFIG_BOOL, 0 },
  { "plugins", CONFIG_BOOL, 0 },
  { "goal_check", CONFIG_BOOL, 0 },
  { "max_steps", CONFIG_INT, 0 },
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

int motiris_set_config(const char *key, const char *value) {
  const ConfigKey *spec = NULL;
  if (key && value) {
    for (size_t i = 0; i < sizeof CONFIG_KEYS / sizeof CONFIG_KEYS[0]; i++)
      if (!strcmp(key, CONFIG_KEYS[i].name)) { spec = &CONFIG_KEYS[i]; break; }
  }
  if (!spec) {
    fprintf(stderr, "motiris: unsupported config key (use model, base_url, "
            "api_key_env, transport, system, plugin_dir, shell_allow, "
            "shell_deny, stream, tools, plugins, goal_check or max_steps)\n");
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
    if (*value < '1' || *value > '9') {
      fprintf(stderr, "motiris: max_steps must be an integer in 1..%d\n", INT_MAX);
      return 1;
    }
    char *end = NULL;
    errno = 0;
    int_value = strtol(value, &end, 10);
    if (errno == ERANGE || !end || *end || int_value < 1 || int_value > INT_MAX) {
      fprintf(stderr, "motiris: max_steps must be an integer in 1..%d\n", INT_MAX);
      return 1;
    }
  } else if (!strcmp(key, "transport") && strcmp(value, "auto") &&
             strcmp(value, "libcurl") && strcmp(value, "curl") &&
             strcmp(value, "echo")) {
    fprintf(stderr, "motiris: transport must be auto, libcurl, curl or echo\n");
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