/* memorytools.c - bounded, crash-safe persistent key-value memory.
 *
 * Storage: $HOME/.local/share/motiris/memory.json (plain JSON object).
 * Only a missing file starts empty. Existing invalid input is never replaced
 * implicitly; successful updates use a same-directory atomic rename.
 */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMORY_FILE_MAX (1024 * 1024)
#define MEMORY_KEY_MAX 256
#define MEMORY_VALUE_MAX (256 * 1024)
#define MEMORY_PATH_MAX 4096

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  if (!e) return NULL;
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

static int mkdir_p(const char *path) {
  char *tmp = strdup(path);
  if (!tmp) return -1;
  int rc = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(tmp, 0755) && errno != EEXIST) { rc = -1; *p = '/'; goto out; }
    *p = '/';
  }
  if (mkdir(tmp, 0755) && errno != EEXIST) rc = -1;
out:
  free(tmp);
  return rc;
}

static char *memory_path(int create, const char **error) {
  const char *h = getenv("HOME");
  if (!h) h = ".";
  size_t dir_size = strlen(h) + sizeof("/.local/share/motiris");
  size_t path_size = dir_size + sizeof("/memory.json");
  if (dir_size > MEMORY_PATH_MAX || path_size > MEMORY_PATH_MAX) {
    if (error) *error = "memory path too long";
    return NULL;
  }

  char *dir = malloc(dir_size);
  if (!dir) {
    if (error) *error = "cannot allocate memory path";
    return NULL;
  }
  snprintf(dir, dir_size, "%s/.local/share/motiris", h);
  if (create && mkdir_p(dir)) {
    free(dir);
    if (error) *error = "cannot create memory state directory";
    return NULL;
  }

  char *path = malloc(path_size);
  if (!path) {
    free(dir);
    if (error) *error = "cannot allocate memory path";
    return NULL;
  }
  snprintf(path, path_size, "%s/memory.json", dir);
  free(dir);
  return path;
}

static cJSON *load_fail(const char **error, const char *message) {
  if (error) *error = message;
  return NULL;
}

/* Return an empty object only for ENOENT. Every existing-file failure is
 * explicit so callers cannot mistake damaged data for an empty database. */
static cJSON *load_memory(const char **error) {
  if (error) *error = NULL;
  char *path = memory_path(0, error);
  if (!path) return NULL;

  /* O_NONBLOCK prevents FIFOs and other non-regular files from hanging in
   * open before fstat can reject them. It has no effect on regular files. */
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  int open_errno = errno;
  free(path);
  if (fd < 0) {
    if (open_errno == ENOENT) {
      cJSON *empty = cJSON_CreateObject();
      if (!empty) return load_fail(error, "cannot allocate empty memory");
      return empty;
    }
    return load_fail(error, "cannot read memory file");
  }

  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    return load_fail(error, "memory file is not a regular file");
  }
  if (st.st_size == 0) {
    close(fd);
    return load_fail(error, "memory file is empty");
  }
  if (st.st_size < 0 || st.st_size > MEMORY_FILE_MAX) {
    close(fd);
    return load_fail(error, "memory file exceeds 1 MiB limit");
  }

  size_t expected = (size_t)st.st_size;
  char *buffer = malloc(expected + 2);
  if (!buffer) {
    close(fd);
    return load_fail(error, "cannot allocate memory read buffer");
  }

  size_t used = 0;
  int read_failed = 0;
  while (used <= expected) {
    ssize_t n = read(fd, buffer + used, expected + 1 - used);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { read_failed = 1; break; }
    if (n == 0) break;
    used += (size_t)n;
  }
  if (close(fd)) read_failed = 1;
  if (read_failed || used != expected) {
    free(buffer);
    return load_fail(error, "memory file changed or could not be read completely");
  }
  buffer[used] = '\0';

  cJSON *mem = cJSON_ParseWithLengthOpts(buffer, used + 1, NULL, 1);
  free(buffer);
  if (!cJSON_IsObject(mem)) {
    cJSON_Delete(mem);
    return load_fail(error, "memory file must contain one valid JSON object");
  }
  return mem;
}

static int save_fail(const char **error, const char *message) {
  if (error) *error = message;
  return -1;
}

static int save_memory(cJSON *mem, const char **error) {
  if (error) *error = NULL;
  char *printed = cJSON_PrintUnformatted(mem);
  if (!printed) return save_fail(error, "cannot serialize memory file");
  size_t length = strlen(printed);
  if (length > MEMORY_FILE_MAX) {
    free(printed);
    return save_fail(error, "memory file exceeds 1 MiB limit");
  }

  char *path = memory_path(1, error);
  if (!path) { free(printed); return -1; }
  size_t temporary_size = strlen(path) + sizeof(".tmp.XXXXXX");
  char *temporary = malloc(temporary_size);
  if (!temporary) {
    free(path);
    free(printed);
    return save_fail(error, "cannot allocate temporary memory path");
  }
  snprintf(temporary, temporary_size, "%s.tmp.XXXXXX", path);

  int fd = mkstemp(temporary);
  if (fd < 0) {
    free(temporary);
    free(path);
    free(printed);
    return save_fail(error, "cannot create temporary memory file");
  }

  int rc = -1;
  size_t written = 0;
  while (written < length) {
    ssize_t n = write(fd, printed + written, length - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { save_fail(error, "cannot write temporary memory file"); goto out; }
    written += (size_t)n;
  }
  if (fsync(fd)) { save_fail(error, "cannot sync temporary memory file"); goto out; }
  if (close(fd)) {
    fd = -1;
    save_fail(error, "cannot close temporary memory file");
    goto out;
  }
  fd = -1;
  if (rename(temporary, path)) {
    save_fail(error, "cannot replace memory file atomically");
    goto out;
  }
  rc = 0;

out:
  if (fd >= 0) close(fd);
  if (rc) unlink(temporary);
  free(temporary);
  free(path);
  free(printed);
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
  if (strlen(key) > MEMORY_KEY_MAX) {
    cJSON_Delete(args);
    return err_json("memory key exceeds 256-byte limit");
  }
  if (!value) { cJSON_Delete(args); return err_json("value required (string)"); }
  if (strlen(value) > MEMORY_VALUE_MAX) {
    cJSON_Delete(args);
    return err_json("memory value exceeds 256 KiB limit");
  }

  const char *error = NULL;
  cJSON *mem = load_memory(&error);
  if (!mem) { cJSON_Delete(args); return err_json(error); }

  cJSON_Delete(cJSON_DetachItemFromObject(mem, key));
  if (!cJSON_AddStringToObject(mem, key, value)) {
    cJSON_Delete(mem);
    cJSON_Delete(args);
    return err_json("cannot allocate memory entry");
  }
  int ok = save_memory(mem, &error);
  cJSON_Delete(mem);
  cJSON_Delete(args);

  if (ok) return err_json(error ? error : "cannot persist memory file");
  cJSON *r = cJSON_CreateObject();
  if (!r) return NULL;
  cJSON_AddStringToObject(r, "ok", "stored");
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
  if (key && strlen(key) > MEMORY_KEY_MAX) {
    cJSON_Delete(args);
    return err_json("memory key exceeds 256-byte limit");
  }

  const char *error = NULL;
  cJSON *mem = load_memory(&error);
  if (!mem) { cJSON_Delete(args); return err_json(error); }
  cJSON *r = cJSON_CreateObject();
  if (!r) {
    cJSON_Delete(mem);
    cJSON_Delete(args);
    return NULL;
  }
  if (key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(mem, key);
    if (cJSON_IsString(v))
      cJSON_AddStringToObject(r, "value", v->valuestring);
    else
      cJSON_AddStringToObject(r, "value", "");
  } else {
    cJSON *copy = cJSON_Duplicate(mem, 1);
    if (!copy) {
      cJSON_Delete(r);
      cJSON_Delete(mem);
      cJSON_Delete(args);
      return err_json("cannot copy memory index");
    }
    cJSON_AddItemToObject(r, "keys", copy);
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
