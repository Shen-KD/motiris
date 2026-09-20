/* filetools.c - file read/write/patch/search tools (issue #4).
 *
 * All tools resolve paths against a workspace root (env MOTIRIS_WORKSPACE
 * if set, else the process cwd) and refuse to touch anything outside it
 * (realpath prefix check). Results are plain JSON for the model.
 */
#include "motiris.h"

#include <dirent.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE (1 << 20)      /* 1 MB cap per read */
#define MAX_RESULTS 200

static const char *workspace(void) {
  static char wd[PATH_MAX];
  const char *e = getenv("MOTIRIS_WORKSPACE");
  if (e && *e) { snprintf(wd, sizeof wd, "%s", e); return wd; }
  if (getcwd(wd, sizeof wd)) return wd;
  return ".";
}

/* resolve path; returns malloc'd absolute path or NULL (blocked/noexist) */
static char *resolve(const char *path, char *errbuf, size_t errlen) {
  if (!path || !*path) {
    snprintf(errbuf, errlen, "empty path");
    return NULL;
  }
  char abs[PATH_MAX];
  if (path[0] == '/') snprintf(abs, sizeof abs, "%s", path);
  else snprintf(abs, sizeof abs, "%s/%s", workspace(), path);

  char *res = realpath(abs, NULL);
  if (!res) { snprintf(errbuf, errlen, "cannot resolve %s", path); return NULL; }

  const char *ws = workspace();
  char *wsr = realpath(ws, NULL);
  if (!wsr) { free(res); snprintf(errbuf, errlen, "workspace not resolvable"); return NULL; }
  size_t wl = strlen(wsr);
  int ok = !strncmp(res, wsr, wl) &&
           (res[wl] == '\0' || res[wl] == '/');
  free(wsr);
  if (!ok) {
    snprintf(errbuf, errlen, "path outside workspace: %s", path);
    free(res);
    return NULL;
  }
  return res;
}

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

/* ---------------- read_file ---------------- */
static char *read_file_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *path = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "path"));
  char errb[256];
  char *abs = path ? resolve(path, errb, sizeof errb) : NULL;
  if (!abs) { cJSON_Delete(args); return err_json(errb); }

  FILE *f = fopen(abs, "r");
  free(abs);
  cJSON_Delete(args);
  if (!f) return err_json("cannot open file");

  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += n;
    if (len + 1 >= cap) {
      if (cap >= MAX_FILE) { fclose(f); free(buf);
        return err_json("file exceeds 1MB limit"); }
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }
  fclose(f);
  buf[len] = '\0';
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "content", buf);
  free(buf);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- search_files (recursive grep) ---------------- */
typedef struct { char pat[256]; regex_t re; cJSON *out; int count; } SearchCtx;

static void search_file(const char *path, SearchCtx *sc) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  char *line = NULL;
  size_t cap = 0;
  long lineno = 0;
  while (getline(&line, &cap, f) >= 0) {
    lineno++;
    size_t llen = strlen(line);
    while (llen && (line[llen-1] == '\n' || line[llen-1] == '\r'))
      line[--llen] = '\0';
    if (!regexec(&sc->re, line, 0, NULL, 0)) {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "file", path);
      cJSON_AddNumberToObject(m, "line", (double)lineno);
      cJSON_AddStringToObject(m, "text", line);
      cJSON_AddItemToArray(sc->out, m);
      if (++sc->count >= MAX_RESULTS) { free(line); fclose(f); return; }
    }
  }
  free(line);
  fclose(f);
}

static void walk_dir(const char *dir, SearchCtx *sc) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && sc->count < MAX_RESULTS) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) walk_dir(p, sc);
    else if (S_ISREG(st.st_mode)) search_file(p, sc);
  }
  closedir(d);
}

static char *search_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *pattern = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "pattern"));
  const char *path = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "path"));
  if (!pattern || !*pattern) {
    cJSON_Delete(args);
    return err_json("pattern required");
  }

  char errb[256];
  char *abs = path ? resolve(path, errb, sizeof errb)
                   : strdup(workspace());
  if (!abs) { cJSON_Delete(args); return err_json(errb); }

  SearchCtx sc = {0};
  snprintf(sc.pat, sizeof sc.pat, "%s", pattern);
  sc.out = cJSON_CreateArray();
  if (regcomp(&sc.re, sc.pat, REG_EXTENDED | REG_NOSUB) != 0) {
    cJSON_Delete(sc.out);
    free(abs);
    cJSON_Delete(args);
    return err_json("invalid regex");
  }

  struct stat st;
  if (stat(abs, &st) == 0) {
    if (S_ISDIR(st.st_mode)) walk_dir(abs, &sc);
    else search_file(abs, &sc);
  }
  regfree(&sc.re);
  free(abs);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "matches", sc.out);
  cJSON_AddNumberToObject(r, "count", (double)sc.count);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- write_file ---------------- */
static char *write_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *path = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "path"));
  const char *content = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "content"));
  char errb[256];
  char *abs = path ? resolve(path, errb, sizeof errb) : NULL;
  if (!abs) { cJSON_Delete(args); return err_json(errb); }

  FILE *f = fopen(abs, "w");
  free(abs);
  cJSON_Delete(args);
  if (!f) return err_json("cannot write file");
  if (content) fwrite(content, 1, strlen(content), f);
  fclose(f);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "ok", "written");
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- patch (first occurrence replace) ---------------- */
static char *patch_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *path = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "path"));
  const char *old_t = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "old"));
  const char *new_t = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "new"));
  char errb[256];

  if (!old_t || !*old_t) {
    cJSON_Delete(args);
    return err_json("old text required");
  }
  char *abs = path ? resolve(path, errb, sizeof errb) : NULL;
  if (!abs) { cJSON_Delete(args); return err_json(errb); }

  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  FILE *f = fopen(abs, "r");
  if (!f) { free(buf); free(abs); cJSON_Delete(args);
    return err_json("cannot open file"); }
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += n;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  fclose(f);
  buf[len] = '\0';

  char *hit = strstr(buf, old_t);
  size_t olen = strlen(old_t), nlen = new_t ? strlen(new_t) : 0;
  int replaced = 0;
  if (hit) {
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

  if (replaced) {
    f = fopen(abs, "w");
    if (f) { fwrite(buf, 1, len, f); fclose(f); }
  }
  free(buf);
  free(abs);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "replaced", replaced);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- registration ---------------- */
static const MotirisTool file_tools[] = {
  { "read_file",
    "Read a text file inside the workspace (env MOTIRIS_WORKSPACE or cwd).",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}",
    read_file_call, NULL },
  { "write_file",
    "Overwrite a file inside the workspace with the given content.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
    write_call, NULL },
  { "patch",
    "Replace the first occurrence of 'old' with 'new' in a workspace file.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"old\"]}",
    patch_call, NULL },
  { "search_files",
    "Regex-search files under a path (default workspace root); returns "
    "matching file/line/text entries.",
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
    search_call, NULL },
};

void motiris_register_file_tools(MotirisAgent *a) {
  for (size_t i = 0; i < sizeof file_tools / sizeof file_tools[0]; i++)
    motiris_register_tool(a, &file_tools[i]);
}