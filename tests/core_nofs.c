/* Portable core-loop probe: strict C11, no filesystem/POSIX/libcurl.
 * These three small platform adapters replace host tool/schema registries;
 * the production core objects under test are not special test versions. */
#include "motiris.h"

#include <stdio.h>
#include <string.h>

int motiris_register_core_tools_count(void) { return 0; }
const MotirisTool *motiris_core_tools(void) { return NULL; }
const char *motiris_tool_schema(const char *name) {
  (void)name;
  return NULL;
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
  motiris_free(a);
  puts("core-nofs: strict C11 echo, tool bounds and complete-turn retention passed");
  return 0;
}
