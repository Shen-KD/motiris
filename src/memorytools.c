/* memorytools.c - persistent key-value memory (issue #6).
 *
 * Storage: $HOME/.local/share/motiris/memory.json (plain JSON object).
 * Tools let the model remember facts across sessions; memory_list lists
 * entries so the model can decide what is relevant.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static char *memory_path(void) {
  const char *h = getenv("HOME");
  if (!h) h = ".";
  size_t n = strlen(h) + 48;
  char *dir = malloc(n);
  snprintf(dir, n, "%s/.local/share/motiris", h);
  mkdir_p(dir);
  char *p = malloc(n);
  snprintf(p, n, "%s/memory.json", dir);
  free(dir);
  return p;
}

static cJSON *load_memory(void) {
  char *p = memory_path();
  FILE *f = fopen(p, "r");
  free(p);
  if (!f) return cJSON_CreateObject();
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1 << 20) { fclose(f); return cJSON_CreateObject(); }
  char *buf = malloc((size_t)sz + 1);
  buf[fread(buf, 1, (size_t)sz, f)] = '\0';
  fclose(f);
  cJSON *j = cJSON_Parse(buf);
  free(buf);
  return j ? j : cJSON_CreateObject();
}

static int save_memory(cJSON *mem) {
  char *p = memory_path();
  FILE *f = fopen(p, "w");
  int rc = 0;
  if (f) {
    char *s = cJSON_PrintUnformatted(mem);
    rc = fwrite(s, 1, strlen(s), f) == strlen(s) ? 0 : -1;
    free(s);
    fclose(f);
  } else rc = -1;
  free(p);
  return rc;
}

/* ---------------- memory_set ---------------- */
static char *mem_set_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *key = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "key"));
  const char *value = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "value"));
  if (!key || !*key) { cJSON_Delete(args); return err_json("key required"); }

  cJSON *mem = load_memory();
  free(cJSON_DetachItemFromObject(mem, key));
  cJSON_AddStringToObject(mem, key, value ? value : "");
  int ok = save_memory(mem);
  cJSON_Delete(mem);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  if (ok == 0)
    cJSON_AddStringToObject(r, "ok", "stored");
  else
    cJSON_AddStringToObject(r, "error", "cannot persist memory file");
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- memory_get / memory_list ---------------- */
static char *mem_get_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *key = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "key"));

  cJSON *mem = load_memory();
  cJSON *r = cJSON_CreateObject();
  if (key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(mem, key);
    if (cJSON_IsString(v))
      cJSON_AddStringToObject(r, "value", v->valuestring);
    else
      cJSON_AddStringToObject(r, "value", "");
  } else {
    cJSON_AddItemToObject(r, "keys", cJSON_Duplicate(mem, 1));
  }
  cJSON_Delete(mem);
  cJSON_Delete(args);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- registration ---------------- */
static const MotirisTool mem_tools[] = {
  { "memory_set",
    "Store a fact into long-term memory (persists across sessions).",
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
    "\"value\":{\"type\":\"string\"}},\"required\":[\"key\",\"value\"]}",
    mem_set_call, NULL },
  { "memory_get",
    "Read a fact from long-term memory; key omitted lists all keys.",
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}}}",
    mem_get_call, NULL },
};

void motiris_register_memory_tools(MotirisAgent *a) {
  for (size_t i = 0; i < sizeof mem_tools / sizeof mem_tools[0]; i++)
    motiris_register_tool(a, &mem_tools[i]);
}