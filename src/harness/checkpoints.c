/* checkpoints.c - optional host stdio full-turn snapshot backend.
 *
 * Portable serialization and validation live in src/core/checkpoint.c.  This
 * backend only adds bounded reads and POSIX mode-0600 atomic file replacement.
 * Legacy U/A/T logs are deliberately not accepted as restore input.
 */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAPSHOT_MAX_BYTES (1024 * 1024)
#define SNAPSHOT_MAX_PATH 4096
#define SAVE_ERROR "snapshot write failed"
#define READ_ERROR "snapshot read failed"

static int fail(const char **error, const char *message) {
  if (error) *error = message;
  return -1;
}

int motiris_session_save(const char *path, MotirisAgent *agent,
                         const char *session_id, const char **error) {
  if (error) *error = NULL;
  if (!path || !*path || !agent)
    return fail(error, "invalid snapshot save arguments");
  size_t path_len = strlen(path);
  if (path_len > SNAPSHOT_MAX_PATH)
    return fail(error, "snapshot path too long");

  int rc = -1, fd = -1;
  char *temporary = NULL, *printed = NULL;
  size_t length = 0;
  if (motiris_session_export(agent, session_id, &printed, &length, error))
    return -1;

  /* Keep the temporary basename short: gateway IDs can already produce a
   * 244-byte final filename, leaving no room for a suffix under NAME_MAX. */
  const char *slash = strrchr(path, '/');
  size_t dir_len = slash ? (size_t)(slash - path) + 1 : 0;
  temporary = malloc(dir_len + 64);
  if (!temporary) { fail(error, SAVE_ERROR); goto out; }
  snprintf(temporary, dir_len + 64, "%.*s.motiris-checkpoint-%ld.tmp",
           (int)dir_len, path, (long)getpid());
  fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) { fail(error, SAVE_ERROR); goto out; }
  size_t written = 0;
  while (written < length) {
    ssize_t n = write(fd, printed + written, length - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { fail(error, SAVE_ERROR); goto out; }
    written += (size_t)n;
  }
  if (fsync(fd)) { fail(error, SAVE_ERROR); goto out; }
  if (close(fd)) { fd = -1; fail(error, SAVE_ERROR); goto out; }
  fd = -1;
  if (rename(temporary, path)) {
    fail(error, SAVE_ERROR);
    goto out;
  }
  rc = 0;

out:
  if (fd >= 0) close(fd);
  if (rc && temporary) unlink(temporary);
  free(temporary);
  free(printed);
  return rc;
}

int motiris_session_checkpoint(const char *sub, const char *id,
                               MotirisAgent *agent, const char *session_id,
                               const char **error) {
  char *dir = motiris_state_dir(sub);
  if (!dir) return fail(error, "session state directory unavailable");
  size_t length = strlen(dir) + strlen(id) + 8;
  char *path = malloc(length);
  if (!path) { free(dir); return fail(error, "cannot allocate snapshot path"); }
  snprintf(path, length, "%s/%s.log", dir, id);
  free(dir);
  int rc = motiris_session_save(path, agent, session_id, error);
  free(path);
  return rc;
}

static char *read_snapshot(const char *path, size_t *length,
                           const char **error) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) { fail(error, "cannot open session snapshot"); return NULL; }
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd);
    fail(error, "session snapshot is not a regular file");
    return NULL;
  }
  char *buffer = malloc(SNAPSHOT_MAX_BYTES + 2);
  if (!buffer) { close(fd); fail(error, READ_ERROR); return NULL; }
  size_t used = 0;
  while (used <= SNAPSHOT_MAX_BYTES) {
    ssize_t n = read(fd, buffer + used, SNAPSHOT_MAX_BYTES + 1 - used);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { free(buffer); close(fd); fail(error, READ_ERROR); return NULL; }
    if (!n) break;
    used += (size_t)n;
  }
  close(fd);
  if (used > SNAPSHOT_MAX_BYTES) {
    free(buffer);
    fail(error, "snapshot byte budget exceeded");
    return NULL;
  }
  buffer[used] = '\0';
  *length = used;
  return buffer;
}

int motiris_session_load(MotirisAgent *agent, const char *path,
                         const char *expected_session_id,
                         size_t *loaded_messages, const char **error) {
  if (loaded_messages) *loaded_messages = 0;
  if (error) *error = NULL;
  if (!agent || !path || !*path)
    return fail(error, "invalid snapshot restore arguments");
  cJSON *target = motiris_messages(agent);
  if (!cJSON_IsArray(target) || target->child)
    return fail(error, "snapshot restore requires an empty context");

  size_t length = 0;
  char *buffer = read_snapshot(path, &length, error);
  if (!buffer) return -1;
  int rc = motiris_session_import(agent, buffer, length, expected_session_id,
                                  loaded_messages, error);
  free(buffer);
  return rc;
}
