/* skilltools.c - SKILL.md index + on-demand loading (issue #8).
 *
 * Scans a skills directory for *.md files with YAML-ish frontmatter:
 *   ---
 *   name: skill-name
 *   description: when to use...
 *   ---
 * Exposes skill_list (name+description) and skill_load (full text) so
 * the model can discover and pull a skill instead of having everything
 * injected into the system prompt.
 */
#include "motiris.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SKILLS 128

typedef struct { char name[128]; char desc[512]; char file[512]; char *body; } Skill;

static char *skill_dir = NULL;
static Skill skills[MAX_SKILLS];
static int n_skills = 0;

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

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

/* parse "---\nname: x\ndescription: y\n---\nbody" frontmatter */
static void parse_frontmatter(const char *text, char *name, size_t nname,
                              char *desc, size_t ndesc, const char **body) {
  *name = '\0';
  *desc = '\0';
  *body = text;
  if (strncmp(text, "---", 3)) return;
  const char *end = strstr(text + 3, "\n---");
  if (!end) return;
  const char *line = text + 3;
  while (line < end) {
    const char *nl = strchr(line, '\n');
    if (!nl) nl = end;
    size_t ll = (size_t)(nl - line);
    if (ll > 2 && !strncmp(line, "name:", 5)) {
      const char *v = line + 5;
      while (*v == ' ') v++;
      size_t vn = ll - (size_t)(v - line);
      if (vn >= nname) vn = nname - 1;
      memcpy(name, v, vn);
      name[vn] = '\0';
    } else if (ll > 2 && !strncmp(line, "description:", 12)) {
      const char *v = line + 12;
      while (*v == ' ') v++;
      size_t vn = ll - (size_t)(v - line);
      if (vn >= ndesc) vn = ndesc - 1;
      memcpy(desc, v, vn);
      desc[vn] = '\0';
    }
    line = nl + 1;
  }
  *body = end + 4;
  if (**body == '\n') (*body)++;
}

static const MotirisTool skill_tools[];

static char *skill_list_call(const char *args_json, void *ud);
static char *skill_load_call(const char *args_json, void *ud);
static char *skill_patch_call(const char *args_json, void *ud);
static char *skill_write_call(const char *args_json, void *ud);

/* the skill tools, defined before use so the registration below can
 * count them with sizeof */
static const MotirisTool skill_tools[] = {
  { "skill_list",
    "List available skills (name + description of when to use each).",
    "{\"type\":\"object\",\"properties\":{}}",
    skill_list_call, NULL },
  { "skill_load",
    "Load the full text of a skill by name (use skill_list first).",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"required\":[\"name\"]}",
    skill_load_call, NULL },
  { "skill_patch",
    "Replace the first occurrence of 'old' with 'new' in the skill file "
    "named 'name'. Only modifies files inside the configured --skill-dir.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},"
    "\"required\":[\"name\",\"old\"]}",
    skill_patch_call, NULL },
  { "skill_write",
    "Overwrite the skill file named 'name' with the full new content "
    "(frontmatter name/description are re-parsed). Only modifies files "
    "inside --skill-dir.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"name\",\"content\"]}",
    skill_write_call, NULL },
};

void motiris_register_skill_tools(MotirisAgent *a, const char *dir) {
  if (!dir || !*dir) return;

  free(skill_dir);
  skill_dir = strdup(dir);

  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && n_skills < MAX_SKILLS) {
    size_t len = strlen(e->d_name);
    if (len < 4 || strcmp(e->d_name + len - 3, ".md")) continue;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    char *text = read_file_all(path);
    if (!text) continue;
    Skill *s = &skills[n_skills];
    snprintf(s->file, sizeof s->file, "%s", e->d_name);
    const char *body;
    parse_frontmatter(text, s->name, sizeof s->name,
                      s->desc, sizeof s->desc, &body);
    if (!*s->name) {
      snprintf(s->name, sizeof s->name, "%s", e->d_name);
    }
    s->body = strdup(body);
    free(text);
    n_skills++;
  }
  closedir(d);

  for (size_t i = 0; i < sizeof skill_tools / sizeof skill_tools[0]; i++)
    motiris_register_tool(a, &skill_tools[i]);
}

static char *skill_list_call(const char *args_json, void *ud) {
  (void)args_json; (void)ud;
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < n_skills; i++) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "name", skills[i].name);
    cJSON_AddStringToObject(m, "description", skills[i].desc);
    cJSON_AddItemToArray(arr, m);
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "skills", arr);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static char *skill_load_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *name = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "name"));
  if (!name || !*name) { cJSON_Delete(args); return err_json("name required"); }

  for (int i = 0; i < n_skills; i++) {
    if (!strcmp(skills[i].name, name)) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "name", name);
      cJSON_AddStringToObject(r, "content", skills[i].body);
      cJSON_Delete(args);
      char *s = cJSON_PrintUnformatted(r);
      cJSON_Delete(r);
      return s;
    }
  }
  cJSON_Delete(args);
  return err_json("skill not found");
}

int motiris_skill_count(void) { return n_skills; }

const char *motiris_skill_name(int i) {
  return (i >= 0 && i < n_skills) ? skills[i].name : NULL;
}

const char *motiris_skill_desc(int i) {
  return (i >= 0 && i < n_skills) ? skills[i].desc : NULL;
}

/* ---------------- skill_patch / skill_write ---------------- */
static Skill *find_skill(const char *name) {
  for (int i = 0; i < n_skills; i++)
    if (!strcmp(skills[i].name, name)) return &skills[i];
  return NULL;
}

static char *skill_path(const Skill *s) {
  size_t n = strlen(skill_dir) + strlen(s->file) + 2;
  char *p = malloc(n);
  snprintf(p, n, "%s/%s", skill_dir, s->file);
  return p;
}

static char *skill_patch_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *name = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "name"));
  const char *old_t = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "old"));
  const char *new_t = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "new"));
  if (!name || !*name || !old_t || !*old_t) {
    cJSON_Delete(args);
    return err_json("name and old required");
  }
  Skill *s = find_skill(name);
  if (!s) { cJSON_Delete(args); return err_json("skill not found"); }

  char *path = skill_path(s);
  char *buf = read_file_all(path);
  if (!buf) { free(path); cJSON_Delete(args); return err_json("cannot open skill file"); }
  size_t len = strlen(buf);
  char *hit = strstr(buf, old_t);
  int replaced = 0;
  if (hit) {
    size_t olen = strlen(old_t), nlen = new_t ? strlen(new_t) : 0;
    size_t off = (size_t)(hit - buf);
    size_t newlen = len - olen + nlen;
    char *nb = malloc(newlen + 1);
    memcpy(nb, buf, off);
    if (nlen) memcpy(nb + off, new_t, nlen);
    memcpy(nb + off + nlen, buf + off + olen, len - off - olen + 1);
    free(buf);
    buf = nb;
    len = newlen;
    replaced = 1;
  }
  int wrote = 0;
  if (replaced) {
    FILE *f = fopen(path, "w");
    if (f) { fwrite(buf, 1, len, f); fclose(f); wrote = 1; }
  }
  if (wrote) {
    free(s->body);
    s->body = strdup(buf);
  }
  free(buf);
  free(path);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "replaced", replaced);
  char *sout = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return sout;
}

static char *skill_write_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *name = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "name"));
  const char *content = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "content"));
  if (!name || !*name || !content) {
    cJSON_Delete(args);
    return err_json("name and content required");
  }
  Skill *s = find_skill(name);
  if (!s) { cJSON_Delete(args); return err_json("skill not found"); }

  char *path = skill_path(s);
  FILE *f = fopen(path, "w");
  if (!f) { free(path); cJSON_Delete(args); return err_json("cannot write skill file"); }
  fwrite(content, 1, strlen(content), f);
  fclose(f);
  free(path);

  /* re-parse frontmatter: name/desc may have changed, body always does */
  char newname[128] = "", newdesc[512] = "";
  const char *body;
  parse_frontmatter(content, newname, sizeof newname, newdesc, sizeof newdesc, &body);
  if (!*newname) snprintf(newname, sizeof newname, "%s", name);
  snprintf(s->name, sizeof s->name, "%s", newname);
  snprintf(s->desc, sizeof s->desc, "%s", newdesc);
  free(s->body);
  s->body = strdup(body);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "ok", "written");
  cJSON_AddStringToObject(r, "name", s->name);
  cJSON_AddNumberToObject(r, "skills", (double)n_skills);
  char *sout = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return sout;
}