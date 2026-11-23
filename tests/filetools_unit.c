/* Malformed-argument, bounds and atomic-failure tests for filetools.c. */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int fail_open_once;
static int fail_mkstemp_once;
static int fail_write_once;
static int zero_write_once;
static int short_write_once;
static int fail_sync_once;
static int fail_close_once;
static int fail_rename_once;
static int short_read_state;
static int fail_malloc_after = -1;

static int filetools_test_open(const char *path, int flags, ...);
static int filetools_test_mkstemp(char *path);
static ssize_t filetools_test_read(int fd, void *buffer, size_t count);
static ssize_t filetools_test_write(int fd, const void *buffer, size_t count);
static int filetools_test_fsync(int fd);
static int filetools_test_close(int fd);
static int filetools_test_rename(const char *oldpath, const char *newpath);
static void *filetools_test_malloc(size_t size);

#define open filetools_test_open
#define mkstemp filetools_test_mkstemp
#define read filetools_test_read
#define write filetools_test_write
#define fsync filetools_test_fsync
#define close filetools_test_close
#define rename filetools_test_rename
#define malloc filetools_test_malloc
#include "../src/tools/filetools.c"
#undef open
#undef mkstemp
#undef read
#undef write
#undef fsync
#undef close
#undef rename
#undef malloc

static int filetools_test_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  if (fail_open_once) {
    fail_open_once = 0;
    errno = EIO;
    return -1;
  }
  return (flags & O_CREAT) ? open(path, flags, mode) : open(path, flags);
}

static int filetools_test_mkstemp(char *path) {
  if (fail_mkstemp_once) {
    fail_mkstemp_once = 0;
    errno = EIO;
    return -1;
  }
  return mkstemp(path);
}

static ssize_t filetools_test_read(int fd, void *buffer, size_t count) {
  if (short_read_state == 1) {
    short_read_state = 2;
    return read(fd, buffer, count > 2 ? count - 2 : 0);
  }
  if (short_read_state == 2) {
    short_read_state = 0;
    return 0;
  }
  return read(fd, buffer, count);
}

static ssize_t filetools_test_write(int fd, const void *buffer, size_t count) {
  if (fail_write_once) {
    fail_write_once = 0;
    errno = EIO;
    return -1;
  }
  if (zero_write_once) {
    zero_write_once = 0;
    return 0;
  }
  if (short_write_once && count > 1) {
    short_write_once = 0;
    return write(fd, buffer, count / 2);
  }
  return write(fd, buffer, count);
}

static int filetools_test_fsync(int fd) {
  if (fail_sync_once) {
    fail_sync_once = 0;
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}

static int filetools_test_close(int fd) {
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

static int filetools_test_rename(const char *oldpath, const char *newpath) {
  if (fail_rename_once) {
    fail_rename_once = 0;
    errno = EIO;
    return -1;
  }
  return rename(oldpath, newpath);
}

static void *filetools_test_malloc(size_t size) {
  if (fail_malloc_after >= 0) {
    if (!fail_malloc_after) {
      fail_malloc_after = -1;
      errno = ENOMEM;
      return NULL;
    }
    fail_malloc_after--;
  }
  return malloc(size);
}

void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  (void)a;
  (void)t;
}

int motiris_skill_file_readable(const char *canonical_path) {
  (void)canonical_path;
  return 0; /* this unit exercises ordinary workspace paths only */
}

static void die(const char *message) {
  fprintf(stderr, "filetools-unit: FAIL: %s\n", message);
  exit(1);
}

static void reset_faults(void) {
  fail_open_once = 0;
  fail_mkstemp_once = 0;
  fail_write_once = 0;
  zero_write_once = 0;
  short_write_once = 0;
  fail_sync_once = 0;
  fail_close_once = 0;
  fail_rename_once = 0;
  short_read_state = 0;
  fail_malloc_after = -1;
}

static char *call_write(const char *path, const char *content) {
  cJSON *args = cJSON_CreateObject();
  if (!args || !cJSON_AddStringToObject(args, "path", path) ||
      !cJSON_AddStringToObject(args, "content", content))
    die("allocate write arguments");
  char *json = cJSON_PrintUnformatted(args);
  cJSON_Delete(args);
  if (!json) die("print write arguments");
  char *result = write_call(json, NULL);
  free(json);
  if (!result) die("write_file returned NULL");
  return result;
}

static char *call_patch(const char *path, const char *old_text,
                        const char *new_text) {
  cJSON *args = cJSON_CreateObject();
  if (!args || !cJSON_AddStringToObject(args, "path", path) ||
      !cJSON_AddStringToObject(args, "old", old_text) ||
      !cJSON_AddStringToObject(args, "new", new_text))
    die("allocate patch arguments");
  char *json = cJSON_PrintUnformatted(args);
  cJSON_Delete(args);
  if (!json) die("print patch arguments");
  char *result = patch_call(json, NULL);
  free(json);
  if (!result) die("patch returned NULL");
  return result;
}

static void expect_error(const char *result) {
  cJSON *json = cJSON_Parse(result);
  cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
  if (!cJSON_IsObject(json) || !cJSON_IsString(error) || !error->valuestring[0] ||
      cJSON_GetObjectItemCaseSensitive(json, "ok") ||
      cJSON_GetObjectItemCaseSensitive(json, "replaced"))
    die(result);
  cJSON_Delete(json);
}

static void expect_error_message(const char *result, const char *expected) {
  cJSON *json = cJSON_Parse(result);
  cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
  if (!cJSON_IsObject(json) || !cJSON_IsString(error) ||
      strcmp(error->valuestring, expected) || strlen(result) > 160)
    die(result);
  cJSON_Delete(json);
}

static void expect_written(const char *result) {
  cJSON *json = cJSON_Parse(result);
  cJSON *ok = cJSON_GetObjectItemCaseSensitive(json, "ok");
  if (!cJSON_IsString(ok) || strcmp(ok->valuestring, "written")) die(result);
  cJSON_Delete(json);
}

static void expect_replaced(const char *result, int expected) {
  cJSON *json = cJSON_Parse(result);
  cJSON *replaced = cJSON_GetObjectItemCaseSensitive(json, "replaced");
  if (!cJSON_IsBool(replaced) || cJSON_IsTrue(replaced) != expected) die(result);
  cJSON_Delete(json);
}

static void expect_read_content(const char *result, const char *expected) {
  cJSON *json = cJSON_Parse(result);
  cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
  if (!cJSON_IsString(content) || strcmp(content->valuestring, expected))
    die(result);
  cJSON_Delete(json);
}

static void write_raw(const char *path, const void *data, size_t length) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
  if (fd < 0) die("open raw file");
  const char *p = data;
  size_t done = 0;
  while (done < length) {
    ssize_t n = write(fd, p + done, length - done);
    if (n <= 0) die("write raw file");
    done += (size_t)n;
  }
  if (close(fd)) die("close raw file");
}

static void expect_bytes(const char *path, const void *data, size_t length) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open preserved file");
  char *buffer = malloc(length ? length : 1);
  if (!buffer) die("allocate preserved buffer");
  size_t done = 0;
  while (done < length) {
    ssize_t n = read(fd, buffer + done, length - done);
    if (n <= 0) die("read preserved file");
    done += (size_t)n;
  }
  char extra;
  if (read(fd, &extra, 1) != 0 || memcmp(buffer, data, length))
    die("file was not preserved byte-for-byte");
  free(buffer);
  close(fd);
}

static void expect_no_temps(const char *workspace) {
  size_t size = strlen(workspace) + sizeof("/.motiris-tmp-*");
  char *pattern = malloc(size);
  if (!pattern) die("allocate temporary glob");
  snprintf(pattern, size, "%s/.motiris-tmp-*", workspace);
  glob_t matches;
  int rc = glob(pattern, 0, NULL, &matches);
  free(pattern);
  if (rc == 0) {
    globfree(&matches);
    die("temporary file left behind");
  }
  if (rc != GLOB_NOMATCH) die("temporary glob failed");
}

static void check_malformed(const char *victim, const char *original) {
  static const char *bad_read[] = {
    "{}", "{\"path\":null}", "{\"path\":7}", "{\"path\":[]}"
  };
  static const char *bad_write_path[] = {
    "{\"content\":\"x\"}",
    "{\"path\":null,\"content\":\"x\"}",
    "{\"path\":7,\"content\":\"x\"}",
    "{\"path\":[],\"content\":\"x\"}"
  };
  static const char *bad_content[] = {
    "{\"path\":\"victim.txt\"}",
    "{\"path\":\"victim.txt\",\"content\":null}",
    "{\"path\":\"victim.txt\",\"content\":7}",
    "{\"path\":\"victim.txt\",\"content\":[]}"
  };
  static const char *bad_patch_path[] = {
    "{\"old\":\"OLD\",\"new\":\"x\"}",
    "{\"path\":null,\"old\":\"OLD\",\"new\":\"x\"}",
    "{\"path\":7,\"old\":\"OLD\",\"new\":\"x\"}",
    "{\"path\":[],\"old\":\"OLD\",\"new\":\"x\"}"
  };
  static const char *bad_old[] = {
    "{\"path\":\"victim.txt\",\"new\":\"x\"}",
    "{\"path\":\"victim.txt\",\"old\":null,\"new\":\"x\"}",
    "{\"path\":\"victim.txt\",\"old\":7,\"new\":\"x\"}",
    "{\"path\":\"victim.txt\",\"old\":[],\"new\":\"x\"}",
    "{\"path\":\"victim.txt\",\"old\":\"\",\"new\":\"x\"}"
  };
  static const char *bad_new[] = {
    "{\"path\":\"victim.txt\",\"old\":\"OLD\"}",
    "{\"path\":\"victim.txt\",\"old\":\"OLD\",\"new\":null}",
    "{\"path\":\"victim.txt\",\"old\":\"OLD\",\"new\":7}",
    "{\"path\":\"victim.txt\",\"old\":\"OLD\",\"new\":[]}"
  };
#define CHECK_BAD(calls, call, preserve, message) do { \
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls)[0]; i++) { \
      char *result = (call); \
      expect_error_message(result, message); \
      free(result); \
      if (preserve) expect_bytes(victim, original, strlen(original)); \
    } \
    reset_faults(); \
  } while (0)
  CHECK_BAD(bad_read, read_file_call(bad_read[i], NULL), 0,
            "path required (string)");
  CHECK_BAD(bad_write_path, write_call(bad_write_path[i], NULL), 1,
            "path required (string)");
  CHECK_BAD(bad_content, write_call(bad_content[i], NULL), 1,
            "content required (string)");
  CHECK_BAD(bad_patch_path, patch_call(bad_patch_path[i], NULL), 1,
            "path required (string)");
  CHECK_BAD(bad_old, patch_call(bad_old[i], NULL), 1,
            "old text required (non-empty string)");
  CHECK_BAD(bad_new, patch_call(bad_new[i], NULL), 1,
            "new required (string)");
#undef CHECK_BAD
}

static void check_atomic_fault(const char *victim, const char *original,
                               int patch, int fault) {
  write_raw(victim, original, strlen(original));
  reset_faults();
  if (fault == 1) fail_mkstemp_once = 1;
  if (fault == 2) fail_write_once = 1;
  if (fault == 3) zero_write_once = 1;
  if (fault == 4) fail_sync_once = 1;
  if (fault == 5) fail_close_once = 1;
  if (fault == 6) fail_rename_once = 1;
  if (fault == 7) fail_malloc_after = 0;
  if (fault == 8) fail_malloc_after = 1;
  if (fault == 9) fail_malloc_after = 2;
  char *result = patch ? call_patch("victim.txt", "OLD", "replacement")
                       : call_write("victim.txt", "replacement");
  expect_error(result);
  free(result);
  expect_bytes(victim, original, strlen(original));
  expect_no_temps(workspace());
  reset_faults();
}

int main(void) {
  char ws[] = "/tmp/motiris-filetools-unit-XXXXXX";
  if (!mkdtemp(ws)) die("mkdtemp");
  if (setenv("MOTIRIS_WORKSPACE", ws, 1)) die("setenv workspace");
  size_t path_size = strlen(ws) + sizeof("/victim.txt");
  char *victim = malloc(path_size);
  if (!victim) die("allocate victim path");
  snprintf(victim, path_size, "%s/victim.txt", ws);
  static const char original[] = "KEEP-OLD-BYTES";
  write_raw(victim, original, sizeof(original) - 1);

  check_malformed(victim, original);

  char *result = call_write("victim.txt", "");
  expect_written(result);
  free(result);
  expect_bytes(victim, "", 0);
  result = call_write("victim.txt", "OLD and tail");
  expect_written(result);
  free(result);
  result = call_patch("victim.txt", "OLD", "");
  expect_replaced(result, 1);
  free(result);
  expect_bytes(victim, " and tail", 9);
  result = call_patch("victim.txt", "missing", "x");
  expect_replaced(result, 0);
  free(result);
  expect_bytes(victim, " and tail", 9);
  expect_no_temps(ws);

  write_raw(victim, original, sizeof(original) - 1);
  if (chmod(victim, 0640)) die("chmod victim");
  short_write_once = 1;
  result = call_write("victim.txt", "short writes complete");
  expect_written(result);
  free(result);
  expect_bytes(victim, "short writes complete", 21);
  struct stat st;
  if (stat(victim, &st) || (st.st_mode & 0777) != 0640)
    die("atomic replacement did not preserve file mode");

  for (int fault = 1; fault <= 7; fault++)
    check_atomic_fault(victim, original, 0, fault);
  for (int fault = 1; fault <= 9; fault++)
    check_atomic_fault(victim, original, 1, fault);

  write_raw(victim, original, sizeof(original) - 1);
  fail_open_once = 1;
  result = read_file_call("{\"path\":\"victim.txt\"}", NULL);
  expect_error(result);
  free(result);
  fail_open_once = 1;
  result = call_patch("victim.txt", "OLD", "new");
  expect_error(result);
  free(result);
  expect_bytes(victim, original, sizeof(original) - 1);
  short_read_state = 1;
  result = call_patch("victim.txt", "OLD", "new");
  expect_error(result);
  free(result);
  expect_bytes(victim, original, sizeof(original) - 1);
  expect_no_temps(ws);

  result = read_file_call("{\"path\":\"victim.txt\"}", NULL);
  expect_read_content(result, original);
  free(result);

  size_t big_size = MAX_FILE + 2;
  char *big = malloc(big_size);
  if (!big) die("allocate boundary input");
  memset(big, 'n', big_size - 1);
  big[big_size - 1] = '\0';
  big[MAX_FILE] = '\0';
  write_raw(victim, "x", 1);
  result = call_patch("victim.txt", "x", big);
  expect_replaced(result, 1);
  free(result);
  expect_bytes(victim, big, MAX_FILE);
  result = read_file_call("{\"path\":\"victim.txt\"}", NULL);
  expect_read_content(result, big);
  free(result);

  write_raw(victim, "xy", 2);
  result = call_patch("victim.txt", "x", big);
  expect_error(result);
  free(result);
  expect_bytes(victim, "xy", 2);
  big[MAX_FILE] = 'n';
  big[MAX_FILE + 1] = '\0';
  result = call_write("victim.txt", big);
  expect_error(result);
  free(result);
  expect_bytes(victim, "xy", 2);
  write_raw(victim, big, MAX_FILE + 1);
  result = read_file_call("{\"path\":\"victim.txt\"}", NULL);
  expect_error(result);
  free(result);
  result = call_patch("victim.txt", "n", "x");
  expect_error(result);
  free(result);
  expect_bytes(victim, big, MAX_FILE + 1);

  size_t fifo_size = strlen(ws) + sizeof("/fifo");
  char *fifo = malloc(fifo_size);
  if (!fifo) die("allocate FIFO path");
  snprintf(fifo, fifo_size, "%s/fifo", ws);
  if (mkfifo(fifo, 0600)) die("mkfifo");
  result = read_file_call("{\"path\":\"fifo\"}", NULL);
  expect_error(result);
  free(result);
  result = call_patch("fifo", "x", "y");
  expect_error(result);
  free(result);
  result = call_write("fifo", "y");
  expect_error(result);
  free(result);
  expect_no_temps(ws);

  free(big);
  if (unlink(fifo)) die("unlink FIFO");
  free(fifo);
  if (unlink(victim)) die("unlink victim");
  free(victim);
  if (rmdir(ws)) die("rmdir workspace");
  puts("filetools-unit: malformed arguments, bounds and atomic failures passed");
  return 0;
}
