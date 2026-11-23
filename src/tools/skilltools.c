/* skilltools.c - bounded SKILL.md index + on-demand loading.
 *
 * Each source directory may contain legacy root-level *.md files and
 * <name>/SKILL.md entries. Default startup merges user, workspace and explicit
 * directories in that order; a later same-name skill replaces the earlier one.
 */
#include "motiris.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SKILLS 128
#define MAX_SKILL_ENTRIES 4096
#define SKILL_FILE_MAX_BYTES (1024 * 1024)

typedef struct {
  char name[128];
  char desc[512];
  char file[768];
  char *body;
} Skill;

static Skill skills[MAX_SKILLS];
static int n_skills;

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
  struct stat st;
  if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      st.st_size > SKILL_FILE_MAX_BYTES) {
    fclose(f);
    return NULL;
  }
  size_t size = (size_t)st.st_size;
  char *buf = malloc(size + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, size, f);
  int failed = got != size || ferror(f) || memchr(buf, '\0', size) != NULL;
  fclose(f);
  if (failed) { free(buf); return NULL; }
  buf[size] = '\0';
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

static Skill *find_skill(const char *name) {
  for (int i = 0; i < n_skills; i++)
    if (!strcmp(skills[i].name, name)) return &skills[i];
  return NULL;
}

static void upsert_skill(const char *path, const char *fallback_name) {
  char *text = read_file_all(path);
  if (!text) return;
  char name[128], desc[512];
  const char *body;
  parse_frontmatter(text, name, sizeof name, desc, sizeof desc, &body);
  if (!*name) snprintf(name, sizeof name, "%.127s", fallback_name);
  if (!*name || strlen(path) >= sizeof skills[0].file) {
    free(text);
    return;
  }
  char *body_copy = strdup(body);
  if (!body_copy) { free(text); return; }

  char *canonical = realpath(path, NULL);
  const char *stored = canonical ? canonical : path;
  if (strlen(stored) >= sizeof skills[0].file) {
    free(canonical);
    free(body_copy);
    free(text);
    return;
  }
  Skill *slot = find_skill(name);
  int is_new = 0;
  if (!slot && n_skills < MAX_SKILLS) {
    slot = &skills[n_skills];
    is_new = 1;
  }
  if (!slot) { free(canonical); free(body_copy); free(text); return; }
  if (is_new) n_skills++;
  free(slot->body);
  slot->body = body_copy;
  snprintf(slot->name, sizeof slot->name, "%s", name);
  snprintf(slot->desc, sizeof slot->desc, "%s", desc);
  snprintf(slot->file, sizeof slot->file, "%s", stored);
  for (char *p = slot->file; *p; p++)
    if ((unsigned char)*p < 32 || (unsigned char)*p == 127) *p = '_';
  free(canonical);
  free(text);
}

static const MotirisTool skill_tools[];

static char *skill_list_call(const char *args_json, void *ud);
static char *skill_load_call(const char *args_json, void *ud);
static char *skill_patch_call(const char *args_json, void *ud);
static char *skill_write_call(const char *args_json, void *ud);

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
    "Replace the first occurrence of 'old' with 'new' in an indexed skill. "
    "Only modifies a regular file already present in the skill index.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},"
    "\"required\":[\"name\",\"old\"]}",
    skill_patch_call, NULL },
  { "skill_write",
    "Atomically overwrite an indexed skill with full new content "
    "(frontmatter name/description are re-parsed).",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"name\",\"content\"]}",
    skill_write_call, NULL },
};

static int agent_has_skill_tools(MotirisAgent *a) {
  for (int i = 0; i < motiris_tool_count(a); i++)
    if (!strcmp(motiris_tool_name(a, i), "skill_list")) return 1;
  return 0;
}

void motiris_register_skill_tools(MotirisAgent *a, const char *dir) {
  if (!dir || !*dir) return;
  DIR *d = opendir(dir);
  if (!d) return;
  size_t entries = 0;
  struct dirent *e;
  while (entries++ < MAX_SKILL_ENTRIES && (e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char path[768];
    size_t len = strlen(e->d_name);
    int legacy = len >= 4 && !strcmp(e->d_name + len - 3, ".md");
    int n = legacy ? snprintf(path, sizeof path, "%s/%s", dir, e->d_name)
                   : snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, e->d_name);
    if (n < 0 || (size_t)n >= sizeof path) continue;
    upsert_skill(path, e->d_name);
  }
  closedir(d);

  if (motiris_tools_enabled(a) && n_skills && !agent_has_skill_tools(a))
    for (size_t i = 0; i < sizeof skill_tools / sizeof skill_tools[0]; i++)
      motiris_register_tool(a, &skill_tools[i]);
}

void motiris_register_default_skill_tools(MotirisAgent *a,
                                          const char *explicit_dir) {
  char user[4096], workspace[4096], cwd[3072];
  const char *config = getenv("MOTIRIS_HOME");
  const char *home = getenv("HOME");
  if (config && *config) snprintf(user, sizeof user, "%s/skills", config);
  else if (home && *home)
    snprintf(user, sizeof user, "%s/.motiris/skills", home);
  else user[0] = '\0';
  const char *ws = getenv("MOTIRIS_WORKSPACE");
  if (!ws || !*ws) ws = getcwd(cwd, sizeof cwd) ? cwd : ".";
  snprintf(workspace, sizeof workspace, "%s/.motiris/skills", ws);

  if (*user) motiris_register_skill_tools(a, user);
  if (strcmp(user, workspace)) motiris_register_skill_tools(a, workspace);
  if (explicit_dir && *explicit_dir)
    motiris_register_skill_tools(a, explicit_dir);
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
  Skill *skill = find_skill(name);
  if (!skill) { cJSON_Delete(args); return err_json("skill not found"); }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "name", skill->name);
  cJSON_AddStringToObject(r, "content", skill->body);
  cJSON_Delete(args);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

void motiris_append_skill_context(MotirisAgent *a) {
  if (!n_skills) return;
  size_t cap = 4096, len = 0;
  char *context = malloc(cap);
  if (!context) return;
  const char *heading =
      "# Available skills\n"
      "Select relevant skills by title/description, then call read_file with "
      "the exact path to load SKILL.md on demand. Do not load every skill.\n";
  snprintf(context, cap, "%s", heading);
  len = strlen(context);
  for (int i = 0; i < n_skills && len < 256 * 1024; i++) {
    char row[2048];
    int n = snprintf(row, sizeof row,
                     "- title: %s\n  description: %s\n  path: %s\n",
                     skills[i].name,
                     *skills[i].desc ? skills[i].desc : "(no description)",
                     skills[i].file);
    if (n < 0) continue;
    size_t size = (size_t)n;
    if (size >= sizeof row || size > 256 * 1024 - len) continue;
    if (len + size + 1 > cap) {
      size_t next = cap * 2;
      char *grown = realloc(context, next);
      if (!grown) break;
      context = grown;
      cap = next;
    }
    memcpy(context + len, row, size + 1);
    len += size;
  }
  motiris_append_system(a, context);
  free(context);
}

int motiris_skill_file_readable(const char *absolute_path) {
  if (!absolute_path) return 0;
  for (int i = 0; i < n_skills; i++)
    if (!strcmp(skills[i].file, absolute_path)) return 1;
  return 0;
}

int motiris_skill_count(void) { return n_skills; }
const char *motiris_skill_name(int i) {
  return (i >= 0 && i < n_skills) ? skills[i].name : NULL;
}
const char *motiris_skill_desc(int i) {
  return (i >= 0 && i < n_skills) ? skills[i].desc : NULL;
}

static int write_all(int fd, const char *buf, size_t len) {
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

static int write_skill_file(const char *path, const char *content, size_t len) {
  size_t cap = strlen(path) + 16;
  char *temp = malloc(cap);
  if (!temp) return -1;
  snprintf(temp, cap, "%s.tmp.XXXXXX", path);
  int fd = mkstemp(temp);
  if (fd < 0) { free(temp); return -1; }
  struct stat st;
  mode_t mode = 0600;
  if (!stat(path, &st) && S_ISREG(st.st_mode)) mode = st.st_mode & 0777;
  int rc = fchmod(fd, mode) || write_all(fd, content, len) || fsync(fd);
  if (close(fd)) rc = -1;
  if (!rc && rename(temp, path)) rc = -1;
  if (rc) unlink(temp);
  free(temp);
  return rc ? -1 : 0;
}

static void update_skill_text(Skill *skill, const char *text) {
  char name[128], desc[512];
  const char *body;
  parse_frontmatter(text, name, sizeof name, desc, sizeof desc, &body);
  if (*name) snprintf(skill->name, sizeof skill->name, "%s", name);
  snprintf(skill->desc, sizeof skill->desc, "%s", desc);
  char *copy = strdup(body);
  if (copy) { free(skill->body); skill->body = copy; }
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
  Skill *skill = find_skill(name);
  if (!skill) { cJSON_Delete(args); return err_json("skill not found"); }
  char *buf = read_file_all(skill->file);
  if (!buf) { cJSON_Delete(args); return err_json("cannot open skill file"); }
  char *hit = strstr(buf, old_t);
  if (!hit) {
    free(buf);
    cJSON_Delete(args);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "replaced", 0);
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return out;
  }
  size_t len = strlen(buf), olen = strlen(old_t), nlen = new_t ? strlen(new_t) : 0;
  size_t off = (size_t)(hit - buf);
  if (nlen > SKILL_FILE_MAX_BYTES || len - olen > SKILL_FILE_MAX_BYTES - nlen) {
    free(buf);
    cJSON_Delete(args);
    return err_json("skill exceeds 1 MiB limit");
  }
  size_t newlen = len - olen + nlen;
  char *updated = malloc(newlen + 1);
  if (!updated) { free(buf); cJSON_Delete(args); return err_json("out of memory"); }
  memcpy(updated, buf, off);
  if (nlen) memcpy(updated + off, new_t, nlen);
  memcpy(updated + off + nlen, buf + off + olen, len - off - olen + 1);
  int rc = write_skill_file(skill->file, updated, newlen);
  if (!rc) update_skill_text(skill, updated);
  free(updated);
  free(buf);
  cJSON_Delete(args);
  if (rc) return err_json("cannot write skill file");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "replaced", 1);
  char *out = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return out;
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
  Skill *skill = find_skill(name);
  if (!skill) { cJSON_Delete(args); return err_json("skill not found"); }
  size_t len = strlen(content);
  if (len > SKILL_FILE_MAX_BYTES) {
    cJSON_Delete(args);
    return err_json("skill exceeds 1 MiB limit");
  }
  int rc = write_skill_file(skill->file, content, len);
  if (!rc) update_skill_text(skill, content);
  cJSON_Delete(args);
  if (rc) return err_json("cannot write skill file");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "ok", "written");
  cJSON_AddStringToObject(r, "name", skill->name);
  cJSON_AddNumberToObject(r, "skills", (double)n_skills);
  char *out = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return out;
}
