/* Portable core-loop probe: strict C11, no filesystem/POSIX/libcurl.
 * These three small platform adapters replace host tool/schema registries;
 * the production core objects under test are not special test versions. */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int motiris_register_core_tools_count(void) { return 0; }
const MotirisTool *motiris_core_tools(void) { return NULL; }
const char *motiris_tool_schema(const char *name) {
  (void)name;
  return NULL;
}

static int cancel_flag;
static int cancel_requested(void *ud) {
  (void)ud;
  return cancel_flag;
}
static char *shell_result(const char *args, void *ud) {
  (void)args; (void)ud;
  const char result[] = "{\"ok\":true}";
  char *copy = malloc(sizeof result);
  if (copy) memcpy(copy, result, sizeof result);
  return copy;
}
static void cancel_after_tool(const char *name, const char *args,
                              const char *result, void *ud) {
  (void)name; (void)args; (void)result; (void)ud;
  cancel_flag = 1;
}

int main(void) {
  MotirisAgent *a = motiris_new();
  if (!a) return 1;
  motiris_set_transport(a, "echo");
  motiris_set_tools_enabled(a, 0);
  motiris_add_user(a, "offline nofs probe");
  if (motiris_run(a)) {
    fprintf(stderr, "core-nofs: %s\n", motiris_last_error(a));
    motiris_free(a);
    return 1;
  }

  int found_bounded_error = 0;
  cJSON *msgs = motiris_messages(a);
  for (cJSON *m = msgs ? msgs->child : NULL; m; m = m->next) {
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "role"));
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(m, "content"));
    if (role && content && !strcmp(role, "tool") &&
        strstr(content, "unknown tool") && strlen(content) < 128)
      found_bounded_error = 1;
  }
  if (!found_bounded_error) {
    fprintf(stderr, "core-nofs: bounded unavailable-tool error missing\n");
    motiris_free(a);
    return 1;
  }
  if (motiris_set_history_limits(a, 4, 1024 * 1024) ||
      motiris_set_replay_limits(a, 4, 1024 * 1024)) {
    fprintf(stderr, "core-nofs: cannot set bounded history/replay\n");
    motiris_free(a);
    return 1;
  }
  motiris_add_user(a, "second retained turn");
  if (motiris_run(a)) {
    fprintf(stderr, "core-nofs retention: %s\n", motiris_last_error(a));
    motiris_free(a);
    return 1;
  }
  msgs = motiris_messages(a);
  cJSON *first = msgs ? msgs->child : NULL;
  const char *role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(first, "role"));
  const char *content = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(first, "content"));
  if (cJSON_GetArraySize(msgs) != 4 || !role || strcmp(role, "user") ||
      !content || strcmp(content, "second retained turn")) {
    fprintf(stderr, "core-nofs: complete-turn retention failed\n");
    motiris_free(a);
    return 1;
  }

  /* Caller-owned memory is a complete optional backend: no host file symbols
   * or process-global snapshot store are needed by the portable core. */
  const char *error = NULL;
  char *snapshot = NULL, *roundtrip = NULL, *tampered = NULL;
  size_t snapshot_len = 0, roundtrip_len = 0, loaded = 0;
  if (motiris_session_export(a, "nofs-memory", &snapshot, &snapshot_len,
                             &error) ||
      !snapshot || snapshot_len == 0 || snapshot_len > 1024 * 1024 ||
      snapshot[snapshot_len] != '\0') {
    fprintf(stderr, "core-nofs: memory export failed: %s\n",
            error ? error : "invalid buffer");
    goto fail;
  }

  MotirisAgent *b = motiris_new();
  MotirisAgent *c = motiris_new();
  if (!b || !c) goto fail;
  if (motiris_session_import(b, snapshot, snapshot_len, "nofs-memory",
                             &loaded, &error) ||
      loaded != 4 || cJSON_GetArraySize(motiris_messages(b)) != 4) {
    fprintf(stderr, "core-nofs: memory import failed: %s\n",
            error ? error : "message count mismatch");
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  if (motiris_session_export(b, "nofs-memory", &roundtrip, &roundtrip_len,
                             &error) ||
      roundtrip_len != snapshot_len ||
      memcmp(roundtrip, snapshot, snapshot_len + 1)) {
    fprintf(stderr, "core-nofs: memory round-trip changed the snapshot\n");
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }

  if (!motiris_session_import(c, snapshot, snapshot_len, "another-session",
                              &loaded, &error) ||
      !error || !strstr(error, "ownership") || motiris_messages(c)->child) {
    fprintf(stderr, "core-nofs: snapshot ownership was not fail-closed\n");
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  tampered = malloc(snapshot_len + 1);
  if (!tampered) {
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  memcpy(tampered, snapshot, snapshot_len + 1);
  char *digest = strstr(tampered, "\"digest\":\"");
  if (!digest) {
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  digest += strlen("\"digest\":\"");
  digest[0] = digest[0] == '0' ? '1' : '0';
  if (!motiris_session_import(c, tampered, snapshot_len, "nofs-memory",
                              &loaded, &error) ||
      !error || !strstr(error, "integrity") || motiris_messages(c)->child) {
    fprintf(stderr, "core-nofs: snapshot integrity was not fail-closed\n");
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  memcpy(tampered, snapshot, snapshot_len + 1);
  tampered[1] = '\0';
  if (!motiris_session_import(c, tampered, snapshot_len, "nofs-memory",
                              &loaded, &error) ||
      !error || !strstr(error, "embedded NUL") || motiris_messages(c)->child) {
    fprintf(stderr, "core-nofs: embedded NUL was not fail-closed\n");
    motiris_free(b);
    motiris_free(c);
    goto fail;
  }
  motiris_free(b);
  motiris_free(c);
  free(tampered);
  free(roundtrip);
  free(snapshot);

  /* Cancellation after a completed tool call rolls back that call and its
   * assistant message, but retains the active user instruction for a retry. */
  MotirisAgent *d = motiris_new();
  if (!d) { motiris_free(a); return 1; }
  MotirisTool shell = { .name = "shell", .call = shell_result };
  motiris_register_tool(d, &shell);
  motiris_set_transport(d, "echo");
  motiris_add_tool_hook(d, cancel_after_tool, NULL);
  motiris_set_cancel_cb(d, cancel_requested, NULL);
  cancel_flag = 0;
  motiris_add_user(d, "retained interrupted instruction");
  if (motiris_run(d) != MOTIRIS_RUN_CANCELLED ||
      cJSON_GetArraySize(motiris_messages(d)) != 1 ||
      strcmp(motiris_last_error(d), "cancelled")) {
    fprintf(stderr, "core-nofs: interrupted run was not rolled back\n");
    motiris_free(d);
    motiris_free(a);
    return 1;
  }
  motiris_set_cancel_cb(d, NULL, NULL);
  int retry_rc = motiris_run(d);
  int retry_messages = cJSON_GetArraySize(motiris_messages(d));
  if (retry_rc != MOTIRIS_RUN_OK || retry_messages != 4) {
    fprintf(stderr, "core-nofs: retry rc=%d messages=%d error=%s\n",
            retry_rc, retry_messages, motiris_last_error(d));
    motiris_free(d);
    motiris_free(a);
    return 1;
  }
  motiris_free(d);
  motiris_free(a);
  puts("core-nofs: strict C11 echo, cancellation, retention and snapshots passed");
  return 0;

fail:
  free(tampered);
  free(roundtrip);
  free(snapshot);
  motiris_free(a);
  return 1;
}
