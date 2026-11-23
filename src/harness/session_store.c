/* session_store.c - automatic UUID traces and a bounded atomic JSON index.
 *
 * Default layout:
 *   $HOME/.motiris/session/session.json
 *   $HOME/.motiris/session/session-<uuid>.json
 * MOTIRIS_HOME replaces $HOME/.motiris for custom roots and isolated tests.
 */
#include "motiris.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SESSION_INDEX_MAX_BYTES (1024 * 1024)
#define SESSION_PATH_MAX 4096

static int session_fail(const char **error, const char *message) {
  if (error) *error = message;
  return -1;
}

#ifndef MOTIRIS_NO_PERSISTENCE
static int valid_uuid(const char *id) {
  if (!id || strlen(id) != 36) return 0;
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return 0;
    } else if (!((id[i] >= '0' && id[i] <= '9') ||
                 (id[i] >= 'a' && id[i] <= 'f'))) return 0;
  }
  return 1;
}

#endif /* UUID validation is persistence-only */

char *motiris_session_auto_id(void) {
  static unsigned long counter;
  struct timespec now = {0};
  clock_gettime(CLOCK_MONOTONIC, &now);
  unsigned long long a = ((unsigned long long)time(NULL) << 20) ^
                         (unsigned long long)now.tv_nsec;
  unsigned long long b = ((unsigned long long)(unsigned long)getpid() << 32) ^
                         ++counter;
  unsigned char bytes[16];
  for (int i = 0; i < 8; i++) {
    bytes[i] = (unsigned char)(a >> (i * 8));
    bytes[8 + i] = (unsigned char)(b >> (i * 8));
  }
  bytes[6] = (unsigned char)((bytes[6] & 15) | 64);
  bytes[8] = (unsigned char)((bytes[8] & 63) | 128);
  char *id = malloc(37);
  if (id)
    snprintf(id, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
             bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
  return id;
}

#ifndef MOTIRIS_NO_PERSISTENCE
static char *session_root(int create) {
  const char *custom = getenv("MOTIRIS_HOME");
  const char *home = getenv("HOME");
  char *root = malloc(SESSION_PATH_MAX);
  if (!root) return NULL;
  if (custom && *custom)
    snprintf(root, SESSION_PATH_MAX, "%s/session", custom);
  else if (home && *home)
    snprintf(root, SESSION_PATH_MAX, "%s/.motiris/session", home);
  else {
    free(root);
    return NULL;
  }
  if (strlen(root) >= SESSION_PATH_MAX - 64) {
    free(root);
    return NULL;
  }
  if (create) {
    char partial[SESSION_PATH_MAX];
    snprintf(partial, sizeof partial, "%s", root);
    for (char *p = partial + 1; *p; p++) {
      if (*p == '/') {
        *p = '\0';
        if (mkdir(partial, 0755) && errno != EEXIST) {
          free(root);
          return NULL;
        }
        *p = '/';
      }
    }
    if (mkdir(partial, 0755) && errno != EEXIST) {
      free(root);
      return NULL;
    }
  }
  return root;
}

static char *session_path(const char *root, const char *name) {
  size_t size = strlen(root) + strlen(name) + 2;
  char *path = malloc(size);
  if (path) snprintf(path, size, "%s/%s", root, name);
  return path;
}

static cJSON *empty_index(void) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddStringToObject(root, "active_session", "");
  cJSON_AddItemToObject(root, "sessions", cJSON_CreateArray());
  return root;
}

static cJSON *read_index(const char *root) {
  char *path = session_path(root, "session.json");
  if (!path) return NULL;
  FILE *f = fopen(path, "r");
  free(path);
  if (!f) return errno == ENOENT ? empty_index() : NULL;
  struct stat st;
  cJSON *index = NULL;
  if (!fstat(fileno(f), &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
      st.st_size <= SESSION_INDEX_MAX_BYTES) {
    size_t size = (size_t)st.st_size;
    char *text = malloc(size + 1);
    if (text) {
      size_t got = fread(text, 1, size, f);
      text[got] = '\0';
      if (got == size && !ferror(f)) index = cJSON_Parse(text);
      free(text);
    }
  }
  fclose(f);
  if (!cJSON_IsObject(index) ||
      !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(index, "sessions"))) {
    cJSON_Delete(index);
    return NULL;
  }
  return index;
}

static int write_all(int fd, const char *text, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    ssize_t n = write(fd, text + offset, length - offset);
    if (n > 0) { offset += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    if (!n) errno = EIO;
    return -1;
  }
  return 0;
}

static int write_index(const char *root, cJSON *index) {
  char *printed = cJSON_PrintUnformatted(index);
  char *temp = session_path(root, ".session-index-XXXXXX");
  int fd = -1;
  int rc = -1;
  if (printed && temp) fd = mkstemp(temp);
  if (fd >= 0) {
    size_t length = strlen(printed);
    if (length <= SESSION_INDEX_MAX_BYTES && !fchmod(fd, 0600) &&
        !write_all(fd, printed, length) && !fsync(fd) && !close(fd)) {
      fd = -1;
      char *path = session_path(root, "session.json");
      if (path && !rename(temp, path)) rc = 0;
      free(path);
    } else {
      if (fd >= 0) close(fd);
      fd = -1;
    }
  }
  if (fd >= 0) close(fd);
  if (rc && temp) unlink(temp);
  free(temp);
  free(printed);
  return rc;
}

static int lock_index(const char *root) {
  char *path = session_path(root, ".session.lock");
  if (!path) return -1;
  int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
  free(path);
  if (fd < 0) return -1;
  for (int attempt = 0; attempt < 50; attempt++) {
    if (!flock(fd, LOCK_EX | LOCK_NB)) return fd;
    if (errno != EWOULDBLOCK) break;
    usleep(20000);
  }
  close(fd);
  return -1;
}

static void unlock_index(int fd) {
  if (fd < 0) return;
  flock(fd, LOCK_UN);
  close(fd);
}

static int kind_matches(const cJSON *entry, const char *kind,
                        const char *chat_id) {
  const char *entry_kind = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(entry, "kind"));
  const char *entry_chat = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(entry, "chat_id"));
  if (!entry_kind || !kind || strcmp(entry_kind, kind)) return 0;
  if (!chat_id) return 1;
  return entry_chat && !strcmp(entry_chat, chat_id);
}

static cJSON *find_entry(cJSON *index, const char *id) {
  cJSON *sessions = cJSON_GetObjectItemCaseSensitive(index, "sessions");
  for (cJSON *entry = sessions ? sessions->child : NULL; entry; entry = entry->next) {
    const char *candidate = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(entry, "id"));
    if (candidate && !strcmp(candidate, id)) return entry;
  }
  return NULL;
}

int motiris_session_auto_save(MotirisAgent *agent, const char *uuid,
                              const char *kind, const char *chat_id,
                              const char **error) {
  if (error) *error = NULL;
  if (!agent || !valid_uuid(uuid) || !kind)
    return session_fail(error, "invalid automatic session save arguments");
  char *root = session_root(1);
  if (!root) return session_fail(error, "session directory unavailable");
  char filename[64];
  snprintf(filename, sizeof filename, "session-%s.json", uuid);
  char *trace = session_path(root, filename);
  int rc = -1;
  if (trace && !motiris_session_save(trace, agent, uuid, error)) {
    int lock = lock_index(root);
    if (lock >= 0) {
      cJSON *index = read_index(root);
      cJSON *entry = index ? find_entry(index, uuid) : NULL;
      if (entry) {
        cJSON *sessions = cJSON_GetObjectItemCaseSensitive(index, "sessions");
        cJSON_DetachItemViaPointer(sessions, entry);
        cJSON_Delete(entry);
      }
      cJSON *sessions = index ? cJSON_GetObjectItemCaseSensitive(index, "sessions") : NULL;
      entry = cJSON_CreateObject();
      if (sessions && entry) {
        cJSON_AddStringToObject(entry, "id", uuid);
        cJSON_AddStringToObject(entry, "kind", kind);
        if (chat_id) cJSON_AddStringToObject(entry, "chat_id", chat_id);
        cJSON_AddStringToObject(entry, "file", filename);
        cJSON_AddNumberToObject(entry, "updated_at", (double)time(NULL));
        cJSON_AddNumberToObject(entry, "messages",
                                (double)cJSON_GetArraySize(motiris_messages(agent)));
        cJSON_AddItemToArray(sessions, entry);
        cJSON *active = cJSON_GetObjectItemCaseSensitive(index, "active_session");
        if (active) cJSON_SetValuestring(active, uuid);
        if (!write_index(root, index)) rc = 0;
      } else {
        cJSON_Delete(entry);
      }
      cJSON_Delete(index);
      unlock_index(lock);
    }
    if (rc && error && !*error) *error = "session index update failed";
  }
  free(trace);
  free(root);
  return rc;
}

int motiris_session_auto_restore(MotirisAgent *agent, const char *kind,
                                 const char *chat_id, const char *selector,
                                 char **uuid, size_t *loaded_messages,
                                 const char **error) {
  if (uuid) *uuid = NULL;
  if (loaded_messages) *loaded_messages = 0;
  if (error) *error = NULL;
  if (!agent || !kind || (selector && !valid_uuid(selector)))
    return session_fail(error, "invalid automatic session restore arguments");
  char *root = session_root(0);
  if (!root) return session_fail(error, "session directory unavailable");
  cJSON *index = read_index(root);
  if (!index) {
    free(root);
    return session_fail(error, "session index unavailable or malformed");
  }
  cJSON *sessions = cJSON_GetObjectItemCaseSensitive(index, "sessions");
  cJSON *match = NULL;
  for (cJSON *entry = sessions ? sessions->child : NULL; entry; entry = entry->next) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
    if (valid_uuid(id) && kind_matches(entry, kind, chat_id) &&
        (!selector || !strcmp(selector, id))) match = entry;
  }
  int rc = 1;
  if (match) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(match, "id"));
    char filename[64];
    snprintf(filename, sizeof filename, "session-%s.json", id);
    char *trace = session_path(root, filename);
    if (!trace || motiris_session_load(agent, trace, id, loaded_messages, error)) {
      rc = session_fail(error, error && *error ? *error : "session trace restore failed");
    } else {
      char *copy = strdup(id);
      if (!copy) rc = session_fail(error, "out of memory restoring session id");
      else {
        if (uuid) *uuid = copy;
        else free(copy);
        rc = 0;
      }
    }
    free(trace);
  }
  cJSON_Delete(index);
  free(root);
  return rc;
}

int motiris_session_auto_delete(const char *kind, const char *chat_id,
                                const char **error) {
  if (error) *error = NULL;
  if (!kind) return session_fail(error, "invalid automatic session delete");
  char *root = session_root(0);
  if (!root) return session_fail(error, "session directory unavailable");
  char *index_path = session_path(root, "session.json");
  if (index_path && access(index_path, F_OK) && errno == ENOENT) {
    free(index_path);
    free(root);
    return 0;
  }
  free(index_path);
  int lock = lock_index(root);
  int rc = -1;
  if (lock >= 0) {
    cJSON *index = read_index(root);
    cJSON *sessions = index ? cJSON_GetObjectItemCaseSensitive(index, "sessions") : NULL;
    int changed = 0;
    for (cJSON *entry = sessions ? sessions->child : NULL; entry;) {
      cJSON *next = entry->next;
      if (kind_matches(entry, kind, chat_id)) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
        if (valid_uuid(id)) {
          char filename[64];
          snprintf(filename, sizeof filename, "session-%s.json", id);
          char *trace = session_path(root, filename);
          if (trace) unlink(trace);
          free(trace);
        }
        cJSON_DetachItemViaPointer(sessions, entry);
        cJSON_Delete(entry);
        changed = 1;
      }
      entry = next;
    }
    if (index) {
      cJSON *last = sessions && sessions->child ? sessions->child : NULL;
      while (last && last->next) last = last->next;
      const char *active = last ? cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(last, "id")) : "";
      cJSON *node = cJSON_GetObjectItemCaseSensitive(index, "active_session");
      if (node) cJSON_SetValuestring(node, active ? active : "");
      if (!changed || !write_index(root, index)) rc = 0;
    }
    cJSON_Delete(index);
    unlock_index(lock);
  }
  if (rc) session_fail(error, "session index delete failed");
  free(root);
  return rc;
}

int motiris_session_auto_list(const char *kind, const char *term) {
  char *root = session_root(0);
  if (!root) { printf("no automatic sessions yet\n"); return 0; }
  cJSON *index = read_index(root);
  free(root);
  if (!index) { printf("no automatic sessions yet\n"); return 0; }
  cJSON *sessions = cJSON_GetObjectItemCaseSensitive(index, "sessions");
  int shown = 0;
  for (cJSON *entry = sessions ? sessions->child : NULL; entry; entry = entry->next) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
    const char *entry_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
    const char *chat = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "chat_id"));
    double updated = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "updated_at"));
    double messages = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "messages"));
    if (kind && entry_kind && strcmp(kind, entry_kind)) continue;
    if (term && (!id || !strstr(id, term)) && (!chat || !strstr(chat, term))) continue;
    printf("%s %-8s %-24s %d msgs %lld\n", id ? id : "(invalid)",
           entry_kind ? entry_kind : "?", chat ? chat : "",
           (int)messages, (long long)updated);
    shown = 1;
  }
  if (!shown) printf("no automatic sessions yet\n");
  cJSON_Delete(index);
  return 0;
}

#else /* MOTIRIS_NO_PERSISTENCE */
int motiris_session_auto_save(MotirisAgent *agent, const char *uuid,
                              const char *kind, const char *chat_id,
                              const char **error) {
  (void)agent; (void)uuid; (void)kind; (void)chat_id;
  return session_fail(error, "snapshot persistence unavailable");
}
int motiris_session_auto_restore(MotirisAgent *agent, const char *kind,
                                 const char *chat_id, const char *selector,
                                 char **uuid, size_t *loaded_messages,
                                 const char **error) {
  (void)agent; (void)kind; (void)chat_id; (void)selector;
  if (uuid) *uuid = NULL;
  if (loaded_messages) *loaded_messages = 0;
  return session_fail(error, "snapshot persistence unavailable");
}
int motiris_session_auto_delete(const char *kind, const char *chat_id,
                                const char **error) {
  (void)kind; (void)chat_id;
  return session_fail(error, "snapshot persistence unavailable");
}
int motiris_session_auto_list(const char *kind, const char *term) {
  (void)kind; (void)term;
  printf("automatic session persistence unavailable\n");
  return 0;
}
#endif
