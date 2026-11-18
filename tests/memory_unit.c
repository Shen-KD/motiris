/* Fault-injection tests for the production memorytools.c translation unit. */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int short_read_state;
static int fail_write_once;
static int fail_sync_once;
static int fail_close_once;
static int fail_rename_once;

static ssize_t memory_test_read(int fd, void *buffer, size_t count);
static ssize_t memory_test_write(int fd, const void *buffer, size_t count);
static int memory_test_fsync(int fd);
static int memory_test_close(int fd);
static int memory_test_rename(const char *oldpath, const char *newpath);

#define read memory_test_read
#define write memory_test_write
#define fsync memory_test_fsync
#define close memory_test_close
#define rename memory_test_rename
#include "../src/tools/memorytools.c"
#undef read
#undef write
#undef fsync
#undef close
#undef rename

static ssize_t memory_test_read(int fd, void *buffer, size_t count) {
  if (short_read_state == 1) {
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 2) return -1;
    short_read_state = 2;
    return read(fd, buffer, (size_t)st.st_size - 1);
  }
  if (short_read_state == 2) {
    short_read_state = 0;
    return 0;
  }
  return read(fd, buffer, count);
}

static ssize_t memory_test_write(int fd, const void *buffer, size_t count) {
  if (fail_write_once) {
    fail_write_once = 0;
    errno = EIO;
    return -1;
  }
  return write(fd, buffer, count);
}

static int memory_test_fsync(int fd) {
  if (fail_sync_once) {
    fail_sync_once = 0;
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}

static int memory_test_close(int fd) {
  int flags = fcntl(fd, F_GETFL);
  int inject = fail_close_once && flags >= 0 &&
               (flags & O_ACCMODE) != O_RDONLY;
  int rc = close(fd);
  if (inject) {
    fail_close_once = 0;
    errno = EIO;
    return -1;
  }
  return rc;
}

static int memory_test_rename(const char *oldpath, const char *newpath) {
  if (fail_rename_once) {
    fail_rename_once = 0;
    errno = EIO;
    return -1;
  }
  return rename(oldpath, newpath);
}

void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  (void)a;
  (void)t;
}

static void die(const char *message) {
  fprintf(stderr, "memory-unit: FAIL: %s\n", message);
  exit(1);
}

static char *call_set(const char *key, const char *value) {
  cJSON *args = cJSON_CreateObject();
  if (!args || !cJSON_AddStringToObject(args, "key", key) ||
      !cJSON_AddStringToObject(args, "value", value))
    die("allocate set arguments");
  char *json = cJSON_PrintUnformatted(args);
  cJSON_Delete(args);
  if (!json) die("print set arguments");
  char *result = mem_set_call(json, NULL);
  free(json);
  if (!result) die("memory_set returned NULL");
  return result;
}

static char *call_set_raw(const char *json) {
  char *result = mem_set_call(json, NULL);
  if (!result) die("memory_set returned NULL for raw arguments");
  return result;
}

static char *call_get(const char *key) {
  cJSON *args = cJSON_CreateObject();
  if (!args) die("allocate get arguments");
  if (key && !cJSON_AddStringToObject(args, "key", key))
    die("allocate get key");
  char *json = cJSON_PrintUnformatted(args);
  cJSON_Delete(args);
  if (!json) die("print get arguments");
  char *result = mem_get_call(json, NULL);
  free(json);
  if (!result) die("memory_get returned NULL");
  return result;
}

static void expect_error(const char *result) {
  cJSON *json = cJSON_Parse(result);
  if (!cJSON_IsObject(json) ||
      !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "error")) ||
      cJSON_GetObjectItemCaseSensitive(json, "ok"))
    die(result);
  cJSON_Delete(json);
}

static void expect_error_message(const char *result, const char *expected) {
  cJSON *json = cJSON_Parse(result);
  cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
  if (!cJSON_IsObject(json) || !cJSON_IsString(error) ||
      strcmp(error->valuestring, expected) || strlen(result) > 128)
    die(result);
  cJSON_Delete(json);
}

static void expect_stored(const char *result) {
  cJSON *json = cJSON_Parse(result);
  cJSON *ok = cJSON_GetObjectItemCaseSensitive(json, "ok");
  if (!cJSON_IsString(ok) || strcmp(ok->valuestring, "stored")) die(result);
  cJSON_Delete(json);
}

static void expect_value(const char *result, const char *expected) {
  cJSON *json = cJSON_Parse(result);
  cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
  if (!cJSON_IsString(value) || strcmp(value->valuestring, expected)) die(result);
  cJSON_Delete(json);
}

static char *path_for_writing(void) {
  const char *error = NULL;
  char *path = memory_path(1, &error);
  if (!path) die(error ? error : "memory path");
  return path;
}

static void write_raw(const char *path, const void *data, size_t length) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) die("open raw memory file");
  const char *p = data;
  size_t done = 0;
  while (done < length) {
    ssize_t n = write(fd, p + done, length - done);
    if (n <= 0) die("write raw memory file");
    done += (size_t)n;
  }
  if (close(fd)) die("close raw memory file");
}

static void expect_bytes(const char *path, const void *data, size_t length) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open preserved memory file");
  char *buffer = malloc(length + 1);
  if (!buffer) die("allocate preserved buffer");
  size_t done = 0;
  while (done < length) {
    ssize_t n = read(fd, buffer + done, length - done);
    if (n <= 0) die("read preserved memory file");
    done += (size_t)n;
  }
  char extra;
  if (read(fd, &extra, 1) != 0 || memcmp(buffer, data, length))
    die("memory file was not preserved byte-for-byte");
  free(buffer);
  close(fd);
}

static void check_invalid(const char *path, const void *data, size_t length) {
  write_raw(path, data, length);
  char *result = call_get(NULL);
  expect_error(result);
  free(result);
  result = call_set("new", "value");
  expect_error(result);
  free(result);
  expect_bytes(path, data, length);
}

static char *read_all(const char *path, size_t *length) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open memory snapshot");
  struct stat st;
  if (fstat(fd, &st) || st.st_size < 0) die("stat memory snapshot");
  *length = (size_t)st.st_size;
  char *buffer = malloc(*length ? *length : 1);
  if (!buffer) die("allocate memory snapshot");
  size_t done = 0;
  while (done < *length) {
    ssize_t n = read(fd, buffer + done, *length - done);
    if (n <= 0) die("read memory snapshot");
    done += (size_t)n;
  }
  close(fd);
  return buffer;
}

static void expect_no_temps(const char *path) {
  size_t size = strlen(path) + sizeof(".tmp.*");
  char *pattern = malloc(size);
  if (!pattern) die("allocate temporary glob");
  snprintf(pattern, size, "%s.tmp.*", path);
  glob_t matches;
  int rc = glob(pattern, 0, NULL, &matches);
  free(pattern);
  if (rc == 0) {
    globfree(&matches);
    die("temporary memory file left behind");
  }
  if (rc != GLOB_NOMATCH) die("temporary memory glob failed");
}

static void check_save_failure(const char *path, int fault) {
  static const char original[] = "{\"keep\":\"yes\"}";
  write_raw(path, original, sizeof(original) - 1);
  fail_write_once = fault == 1;
  fail_sync_once = fault == 2;
  fail_close_once = fault == 3;
  fail_rename_once = fault == 4;
  char *result = call_set("new", "value");
  expect_error(result);
  free(result);
  expect_bytes(path, original, sizeof(original) - 1);
  expect_no_temps(path);
}

int main(void) {
  char home[] = "/tmp/motiris-memory-unit-XXXXXX";
  if (!mkdtemp(home)) die("mkdtemp");
  if (setenv("HOME", home, 1)) die("setenv HOME");
  char *path = path_for_writing();

  char *result = call_get(NULL);
  cJSON *empty = cJSON_Parse(result);
  if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(empty, "keys")))
    die("missing file did not start with an empty object");
  cJSON_Delete(empty);
  free(result);

  result = call_set("planet", "mars");
  expect_stored(result);
  free(result);
  result = call_get("planet");
  expect_value(result, "mars");
  free(result);
  for (int i = 0; i < 64; i++) {
    result = call_set("planet", i & 1 ? "earth" : "mars");
    expect_stored(result);
    free(result);
  }
  result = call_get("planet");
  expect_value(result, "earth");
  free(result);
  struct stat st;
  if (stat(path, &st) || (st.st_mode & 0777) != 0600)
    die("memory file permissions are not 0600");

  static const char corrupt[] = "{\"important\":\"keep\",}";
  static const char array[] = "[]";
  static const char trailing[] = "{\"keep\":\"yes\"} trailing";
  check_invalid(path, "", 0);
  check_invalid(path, corrupt, sizeof(corrupt) - 1);
  check_invalid(path, array, sizeof(array) - 1);
  check_invalid(path, trailing, sizeof(trailing) - 1);
  char *oversize = malloc(MEMORY_FILE_MAX + 1);
  if (!oversize) die("allocate oversize input");
  memset(oversize, 'x', MEMORY_FILE_MAX + 1);
  check_invalid(path, oversize, MEMORY_FILE_MAX + 1);
  free(oversize);

  static const char valid[] = "{\"important\":\"keep\"}";
  write_raw(path, valid, sizeof(valid) - 1);
  short_read_state = 1;
  result = call_get(NULL);
  expect_error(result);
  free(result);
  expect_bytes(path, valid, sizeof(valid) - 1);
  short_read_state = 1;
  result = call_set("new", "value");
  expect_error(result);
  free(result);
  expect_bytes(path, valid, sizeof(valid) - 1);

  for (int fault = 1; fault <= 4; fault++) check_save_failure(path, fault);

  static const char *bad_values[] = {
    "{\"key\":\"important\"}",
    "{\"key\":\"important\",\"value\":null}",
    "{\"key\":\"important\",\"value\":7}",
    "{\"key\":\"important\",\"value\":[]}"
  };
  for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); i++) {
    write_raw(path, valid, sizeof(valid) - 1);
    result = call_set_raw(bad_values[i]);
    expect_error_message(result, "value required (string)");
    free(result);
    expect_bytes(path, valid, sizeof(valid) - 1);
    expect_no_temps(path);
  }
  result = call_set("important", "");
  expect_stored(result);
  free(result);
  result = call_get("important");
  expect_value(result, "");
  free(result);

  write_raw(path, valid, sizeof(valid) - 1);
  char *long_key = malloc(MEMORY_KEY_MAX + 2);
  char *long_value = malloc(MEMORY_VALUE_MAX + 2);
  if (!long_key || !long_value) die("allocate limit inputs");
  memset(long_key, 'k', MEMORY_KEY_MAX + 1);
  long_key[MEMORY_KEY_MAX + 1] = '\0';
  memset(long_value, 'v', MEMORY_VALUE_MAX + 1);
  long_value[MEMORY_VALUE_MAX + 1] = '\0';
  result = call_set(long_key, "value");
  expect_error(result);
  free(result);
  expect_bytes(path, valid, sizeof(valid) - 1);
  result = call_get(long_key);
  expect_error(result);
  free(result);
  expect_bytes(path, valid, sizeof(valid) - 1);
  result = call_set("key", long_value);
  expect_error(result);
  free(result);
  expect_bytes(path, valid, sizeof(valid) - 1);
  long_key[MEMORY_KEY_MAX] = '\0';
  long_value[MEMORY_VALUE_MAX] = '\0';
  result = call_set(long_key, long_value);
  expect_stored(result);
  free(result);
  result = call_get(long_key);
  expect_value(result, long_value);
  free(result);
  result = call_set("second", long_value);
  expect_stored(result);
  free(result);
  result = call_set("third", long_value);
  expect_stored(result);
  free(result);
  size_t before_length;
  char *before = read_all(path, &before_length);
  result = call_set("fourth", long_value);
  expect_error(result);
  free(result);
  expect_bytes(path, before, before_length);
  expect_no_temps(path);
  free(before);
  free(long_key);
  free(long_value);

  if (unlink(path)) die("unlink memory file");
  char *slash = strrchr(path, '/');
  if (!slash) die("memory directory path");
  *slash = '\0';
  if (rmdir(path)) die("remove motiris state directory");
  *slash = '\0';
  slash = strrchr(path, '/');
  if (!slash) die("share path");
  *slash = '\0';
  rmdir(path);
  slash = strrchr(path, '/');
  if (!slash) die("local path");
  *slash = '\0';
  rmdir(path);
  free(path);
  rmdir(home);
  puts("memory-unit: corruption, bounds, atomic failures and replacement checks passed");
  return 0;
}
