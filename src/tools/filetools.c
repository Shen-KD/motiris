/* filetools.c - file read/write/patch/search tools (issue #4).
 *
 * All tools resolve paths against a workspace root (env MOTIRIS_WORKSPACE
 * if set, else the process cwd) and refuse to touch anything outside it
 * (realpath prefix check). Results are plain JSON for the model.
 */
#include "motiris.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE (1 << 20)      /* 1 MB cap per read */
#define MAX_RESULTS 200
#define MAX_SEARCH_DEPTH 32
#define MAX_SEARCH_NODES 4096
#define MAX_SEARCH_LINE 4096
#define MAX_SEARCH_FILE_BYTES (1 << 20)
#define MAX_SEARCH_BYTES (16 << 20)

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

/* ---------------- search_files (bounded recursive grep) ---------------- */
typedef struct {
  regex_t re;
  cJSON *out;
  size_t count, nodes, bytes;
  size_t skipped_symlinks, overlong_lines, unreadable;
  int truncated;
  char error[160];
} SearchCtx;

typedef struct { DIR *dir; char *path; } SearchFrame;

static void search_limit(SearchCtx *sc, const char *msg) {
  if (sc->truncated) return;
  sc->truncated = 1;
  snprintf(sc->error, sizeof sc->error, "%s", msg);
}

static char *search_join(const char *dir, const char *name, SearchCtx *sc) {
  size_t dl = strlen(dir), nl = strlen(name);
  if (dl >= PATH_MAX || nl >= PATH_MAX || dl + nl + 2 > PATH_MAX) {
    search_limit(sc, "search path exceeds PATH_MAX");
    return NULL;
  }
  char *p = malloc(dl + nl + 2);
  if (!p) { search_limit(sc, "search path allocation failed"); return NULL; }
  memcpy(p, dir, dl);
  p[dl] = '/';
  memcpy(p + dl + 1, name, nl + 1);
  return p;
}

/* Open a canonical path below a verified fd (or from / for an absolute
 * workspace path). Every component uses O_NOFOLLOW, closing the check/open
 * symlink race instead of merely checking realpath before fopen. */
static int open_relative_nofollow(int wsfd, const char *rel, int flags) {
  int fd;
  if (*rel == '/') {
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (*rel == '/') rel++;
  } else {
    fd = fcntl(wsfd, F_DUPFD_CLOEXEC, 0);
  }
  if (fd < 0) return -1;
  if (!*rel || !strcmp(rel, ".")) return fd;

  const char *p = rel;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (!n || n > NAME_MAX) { close(fd); errno = ENAMETOOLONG; return -1; }
    char part[NAME_MAX + 1];
    memcpy(part, p, n);
    part[n] = '\0';
    int next = openat(fd, part,
                      slash ? O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                            : flags);
    close(fd);
    if (next < 0) return -1;
    fd = next;
    p = slash ? slash + 1 : p + n;
  }
  return fd;
}

static int search_account_bytes(SearchCtx *sc, size_t n, size_t *file_bytes) {
  if (n > MAX_SEARCH_FILE_BYTES - *file_bytes) {
    search_limit(sc, "search per-file byte limit reached");
    return -1;
  }
  if (n > MAX_SEARCH_BYTES - sc->bytes) {
    search_limit(sc, "search total byte limit reached");
    return -1;
  }
  *file_bytes += n;
  sc->bytes += n;
  return 0;
}

/* Takes ownership of fd. Fixed-size line storage and explicit byte budgets
 * keep both getline-style allocations and result growth bounded. */
static void search_file_fd(int fd, const char *path, SearchCtx *sc) {
  FILE *f = fdopen(fd, "r");
  if (!f) { close(fd); sc->unreadable++; return; }
  char line[MAX_SEARCH_LINE];
  size_t file_bytes = 0;
  long lineno = 0;

  while (sc->count < MAX_RESULTS && !sc->truncated &&
         fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    if (search_account_bytes(sc, n, &file_bytes)) break;
    lineno++;
    int complete = n > 0 && line[n - 1] == '\n';
    if (!complete && !feof(f)) {
      int c;
      while ((c = fgetc(f)) != '\n' && c != EOF) {
        if (search_account_bytes(sc, 1, &file_bytes)) break;
      }
      sc->overlong_lines++;
      continue;
    }
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    if (!regexec(&sc->re, line, 0, NULL, 0)) {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "file", path);
      cJSON_AddNumberToObject(m, "line", (double)lineno);
      cJSON_AddStringToObject(m, "text", line);
      cJSON_AddItemToArray(sc->out, m);
      if (++sc->count >= MAX_RESULTS)
        search_limit(sc, "search result limit reached (200)");
    }
  }
  if (ferror(f)) sc->unreadable++;
  fclose(f);
}

static void walk_dir_fd(int startfd, const char *startpath, SearchCtx *sc) {
  DIR *start = fdopendir(startfd);
  if (!start) { close(startfd); sc->unreadable++; return; }
  SearchFrame stack[MAX_SEARCH_DEPTH];
  size_t depth = 1;
  stack[0].dir = start;
  stack[0].path = strdup(startpath);
  if (!stack[0].path) {
    closedir(start);
    search_limit(sc, "search path allocation failed");
    return;
  }

  while (depth && sc->count < MAX_RESULTS && !sc->truncated) {
    SearchFrame *frame = &stack[depth - 1];
    errno = 0;
    struct dirent *e = readdir(frame->dir);
    if (!e) {
      if (errno) sc->unreadable++;
      closedir(frame->dir);
      free(frame->path);
      depth--;
      continue;
    }
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (sc->nodes >= MAX_SEARCH_NODES) {
      search_limit(sc, "search node limit reached (4096)");
      break;
    }
    sc->nodes++;

    struct stat st;
    int parentfd = dirfd(frame->dir);
    if (fstatat(parentfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
      sc->unreadable++;
      continue;
    }
    if (S_ISLNK(st.st_mode)) { sc->skipped_symlinks++; continue; }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;

    char *p = search_join(frame->path, e->d_name, sc);
    if (!p) break;
    int fd = openat(parentfd, e->d_name,
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      if (errno == ELOOP) sc->skipped_symlinks++;
      else sc->unreadable++;
      free(p);
      continue;
    }
    if (fstat(fd, &st)) {
      close(fd);
      free(p);
      sc->unreadable++;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (depth >= MAX_SEARCH_DEPTH) {
        close(fd);
        free(p);
        search_limit(sc, "search depth limit reached (32)");
        break;
      }
      DIR *child = fdopendir(fd);
      if (!child) {
        close(fd);
        free(p);
        sc->unreadable++;
        continue;
      }
      stack[depth].dir = child;
      stack[depth].path = p;
      depth++;
    } else if (S_ISREG(st.st_mode)) {
      search_file_fd(fd, p, sc);  /* owns fd */
      free(p);
    } else {
      close(fd);
      free(p);
    }
  }

  while (depth) {
    depth--;
    closedir(stack[depth].dir);
    free(stack[depth].path);
  }
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
  char *abs = resolve(path ? path : ".", errb, sizeof errb);
  if (!abs) { cJSON_Delete(args); return err_json(errb); }
  char *wsr = realpath(workspace(), NULL);
  if (!wsr) {
    free(abs);
    cJSON_Delete(args);
    return err_json("workspace not resolvable");
  }
  size_t wl = strlen(wsr);
  if (strncmp(abs, wsr, wl) || (abs[wl] && abs[wl] != '/')) {
    free(wsr);
    free(abs);
    cJSON_Delete(args);
    return err_json("path outside workspace");
  }
  const char *rel = abs[wl] ? abs + wl + 1 : ".";

  SearchCtx sc = {0};
  sc.out = cJSON_CreateArray();
  if (regcomp(&sc.re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
    cJSON_Delete(sc.out);
    free(wsr);
    free(abs);
    cJSON_Delete(args);
    return err_json("invalid regex");
  }

  int wsfd = open_relative_nofollow(
      AT_FDCWD, wsr, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int startfd = wsfd < 0 ? -1 : open_relative_nofollow(
      wsfd, rel, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (wsfd >= 0) close(wsfd);
  free(wsr);
  struct stat st;
  if (startfd < 0 || fstat(startfd, &st)) {
    if (startfd >= 0) close(startfd);
    regfree(&sc.re);
    cJSON_Delete(sc.out);
    free(abs);
    cJSON_Delete(args);
    return err_json("cannot open search path without following symlinks");
  }
  if (S_ISDIR(st.st_mode)) walk_dir_fd(startfd, abs, &sc);
  else if (S_ISREG(st.st_mode)) search_file_fd(startfd, abs, &sc);
  else { close(startfd); sc.unreadable++; }
  regfree(&sc.re);
  free(abs);
  cJSON_Delete(args);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "matches", sc.out);
  cJSON_AddNumberToObject(r, "count", (double)sc.count);
  cJSON_AddNumberToObject(r, "nodes", (double)sc.nodes);
  cJSON_AddNumberToObject(r, "bytes", (double)sc.bytes);
  cJSON_AddNumberToObject(r, "skipped_symlinks", (double)sc.skipped_symlinks);
  cJSON_AddNumberToObject(r, "overlong_lines", (double)sc.overlong_lines);
  cJSON_AddNumberToObject(r, "unreadable", (double)sc.unreadable);
  cJSON_AddBoolToObject(r, "truncated", sc.truncated);
  cJSON_AddStringToObject(r, "symlink_policy", "skip");
  if (sc.error[0]) cJSON_AddStringToObject(r, "error", sc.error);
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