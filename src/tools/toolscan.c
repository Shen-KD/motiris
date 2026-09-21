/* toolscan.c - per-tool schema registry (issue: per-tool directory layout).
 *
 * Tools live in directories: <dir>/<tool_name>/<tool_name>.json holds the
 * tool's parameter schema as a JSON Schema document.  At startup we scan
 * the plugin dir and the tools dir, parse every *.json found, validate it
 * and store it keyed by tool name.  When a tool registers
 * (motiris_register_tool) and a JSON schema exists for its name, that
 * schema replaces the embedded one - so schemas can be tweaked without
 * recompiling, and plugins ship their schema next to their .so.
 *
 * Directory layout (tools and plugins share it):
 *   <plugin_dir>/hello/hello.so  hello.h  hello.json
 *   <tools_dir>/shell/shell.json          (override an embedded schema)
 *
 * Invalid JSON files are skipped with a warning; a broken schema only
 * disables the override, never the tool itself.
 */
#include "motiris.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCHEMAS 64

typedef struct { char name[128]; char *json; } SchemaEntry;

/* declared in plugin.c */
char *motiris_plugin_dir_default(void);

static SchemaEntry schemas[MAX_SCHEMAS];
static int n_schemas = 0;

/* read a whole file into a malloc'd, NUL-terminated buffer */
static char *read_file_all(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += n;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  fclose(f);
  buf[len] = '\0';
  return buf;
}

/* add one schema; invalid JSON is skipped with a warning */
static void add_schema(const char *dir, const char *name, const char *json) {
  cJSON *parsed = cJSON_Parse(json);
  if (!parsed) {
    fprintf(stderr, "motiris: schema %s/%s.json: invalid JSON, skipped\n",
            dir, name);
    return;
  }
  cJSON_Delete(parsed);
  if (n_schemas >= MAX_SCHEMAS) {
    fprintf(stderr, "motiris: schema registry full, %s ignored\n", name);
    return;
  }
  SchemaEntry *s = &schemas[n_schemas];
  snprintf(s->name, sizeof s->name, "%.127s", name);
  s->json = strdup(json);
  n_schemas++;
}

/* scan one directory for per-tool subdirs and their <name>/<name>.json */
static void scan_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (strchr(e->d_name, '.')) continue;   /* only bare tool dirs */
    char json_path[1024];
    snprintf(json_path, sizeof json_path, "%s/%s/%s.json",
             dir, e->d_name, e->d_name);
    char *json = read_file_all(json_path);
    if (!json) continue;
    add_schema(dir, e->d_name, json);
    free(json);
  }
  closedir(d);
}

/* tools dir: $MOTIRIS_TOOLS_DIR or $HOME/.motiris/tools (absent = ok) */
char *motiris_tools_dir_default(void) {
  const char *env = getenv("MOTIRIS_TOOLS_DIR");
  if (env && *env) return strdup(env);
  const char *home = getenv("HOME");
  if (home) {
    size_t n = strlen(home) + 32;
    char *p = malloc(n);
    snprintf(p, n, "%s/.motiris/tools", home);
    return p;
  }
  return strdup("./tools");
}

/* scan the plugin dir and the tools dir for <name>/<name>.json schemas;
 * plugin_dir is the CLI-provided one (may be NULL -> default resolution) */
void motiris_tool_scan(const char *plugin_dir) {
  char *pd = plugin_dir && *plugin_dir ? strdup(plugin_dir)
                                       : motiris_plugin_dir_default();
  scan_dir(pd);
  free(pd);
  char *td = motiris_tools_dir_default();
  scan_dir(td);
  free(td);
}

/* external schema for a tool name, or NULL */
const char *motiris_tool_schema(const char *name) {
  if (!name) return NULL;
  for (int i = 0; i < n_schemas; i++)
    if (!strcmp(schemas[i].name, name)) return schemas[i].json;
  return NULL;
}