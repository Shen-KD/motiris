/* checkpoint.c - portable bounded full-turn snapshot serialization.
 *
 * The caller owns the exported memory buffer and chooses whether/how to
 * persist it.  This file has no filesystem, socket, thread or global session
 * state; host stdio and POSIX atomic replacement live in the harness backend.
 */
#include "motiris.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAPSHOT_FORMAT "motiris-full-turn"
#define SNAPSHOT_VERSION 1
#define SNAPSHOT_MAX_BYTES (1024 * 1024)
#define SNAPSHOT_MAX_MESSAGES 256
#define SNAPSHOT_MAX_RECORD_BYTES (256 * 1024)
#define SNAPSHOT_MAX_TOOL_CALLS 256
#define INVALID_STRUCTURE "invalid snapshot structure"
#define SAVE_ERROR "snapshot serialization failed"

static int fail(const char **error, const char *message) {
  if (error) *error = message;
  return -1;
}

static int only_fields(const cJSON *object, const char *const *allowed,
                       size_t count) {
  if (!cJSON_IsObject(object)) return 0;
  for (cJSON *field = object->child; field; field = field->next) {
    size_t i = 0;
    if (!field->string) return 0;
    for (; i < count; i++)
      if (!strcmp(field->string, allowed[i])) break;
    if (i == count) return 0;
    for (cJSON *prior = object->child; prior != field; prior = prior->next)
      if (prior->string && !strcmp(prior->string, field->string)) return 0;
  }
  return 1;
}

static int validate_tool_calls(const cJSON *calls, size_t *total,
                               const char **error) {
  static const char *const call_fields[] = { "id", "type", "function" };
  static const char *const function_fields[] = { "name", "arguments" };
  if (!cJSON_IsArray(calls))
    return fail(error, INVALID_STRUCTURE);
  size_t count = 0;
  for (cJSON *call = calls->child; call; call = call->next) {
    if (++count > SNAPSHOT_MAX_TOOL_CALLS ||
        *total >= SNAPSHOT_MAX_TOOL_CALLS)
      return fail(error, "snapshot tool-call budget exceeded");
    (*total)++;
    if (!only_fields(call, call_fields,
                     sizeof call_fields / sizeof call_fields[0]))
      return fail(error, INVALID_STRUCTURE);
    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(call, "id"));
    const char *type = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(call, "type"));
    cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
    if (!id || !*id || !type || strcmp(type, "function") ||
        !only_fields(function, function_fields,
                     sizeof function_fields / sizeof function_fields[0]))
      return fail(error, INVALID_STRUCTURE);
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(function, "name"));
    const char *arguments = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(function, "arguments"));
    if (!name || !*name || !arguments)
      return fail(error, INVALID_STRUCTURE);
  }
  return 0;
}

/* Validate roles and exact assistant-tool-call/tool-result pairing.  The
 * serialized per-record and aggregate limits also bound export-side input,
 * which can contain messages received from a provider. */
static int validate_messages(const cJSON *messages, const char **error) {
  static const char *const message_fields[] = {
    "role", "content", "tool_calls", "tool_call_id"
  };
  if (!cJSON_IsArray(messages))
    return fail(error, INVALID_STRUCTURE);

  size_t count = 0, bytes = 0, total_calls = 0;
  cJSON *pending = NULL;
  for (cJSON *message = messages->child; message; message = message->next) {
    if (++count > SNAPSHOT_MAX_MESSAGES)
      return fail(error, "snapshot message budget exceeded");
    if (!only_fields(message, message_fields,
                     sizeof message_fields / sizeof message_fields[0]))
      return fail(error, INVALID_STRUCTURE);
    const char *role = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(message, "role"));
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    const char *call_id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(message, "tool_call_id"));
    if (!role || (strcmp(role, "user") && strcmp(role, "assistant") &&
                  strcmp(role, "tool")))
      return fail(error, INVALID_STRUCTURE);

    if (pending && strcmp(role, "tool"))
      return fail(error, INVALID_STRUCTURE);
    if (!strcmp(role, "user")) {
      if (!cJSON_IsString(content) || calls || call_id)
        return fail(error, INVALID_STRUCTURE);
    } else if (!strcmp(role, "assistant")) {
      if ((content && !cJSON_IsString(content) && !cJSON_IsNull(content)) ||
          call_id)
        return fail(error, INVALID_STRUCTURE);
      if (calls && validate_tool_calls(calls, &total_calls, error)) return -1;
      pending = calls ? calls->child : NULL;
    } else {
      if (!cJSON_IsString(content) || calls || !call_id || !*call_id)
        return fail(error, INVALID_STRUCTURE);
      const char *expected = pending ? cJSON_GetStringValue(
          cJSON_GetObjectItemCaseSensitive(pending, "id")) : NULL;
      if (!expected || strcmp(call_id, expected))
        return fail(error, "orphan or mismatched tool result");
      pending = pending->next;
    }

    char *record = cJSON_PrintUnformatted(message);
    if (!record) return fail(error, "cannot serialize message");
    size_t length = strlen(record);
    free(record);
    if (length > SNAPSHOT_MAX_RECORD_BYTES)
      return fail(error, "snapshot record budget exceeded");
    bytes += length;
    if (bytes > SNAPSHOT_MAX_BYTES)
      return fail(error, "snapshot byte budget exceeded");
  }
  cJSON *first = messages->child;
  cJSON *last = first ? first->prev : NULL;
  const char *first_role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(first, "role"));
  const char *last_role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(last, "role"));
  if (pending || !first_role || strcmp(first_role, "user") ||
      !last_role || strcmp(last_role, "assistant"))
    return fail(error, INVALID_STRUCTURE);
  return 0;
}

static char *digest_messages(const cJSON *messages, const char *session_id) {
  char *payload = cJSON_PrintUnformatted(messages);
  if (!payload) return NULL;
  uint64_t hash = UINT64_C(14695981039346656037);
  const unsigned char *p = (const unsigned char *)(session_id ? session_id : "");
  for (; *p; p++) { hash ^= *p; hash *= UINT64_C(1099511628211); }
  hash ^= 0xff; hash *= UINT64_C(1099511628211);
  for (p = (const unsigned char *)payload; *p; p++) {
    hash ^= *p;
    hash *= UINT64_C(1099511628211);
  }
  free(payload);
  char *digest = malloc(17);
  if (digest) snprintf(digest, 17, "%016llx", (unsigned long long)hash);
  return digest;
}

int motiris_session_export(MotirisAgent *agent, const char *session_id,
                           char **snapshot, size_t *length,
                           const char **error) {
  int rc = -1;
  char *printed = NULL, *digest = NULL;
  cJSON *root = NULL, *copy = NULL;
  if (snapshot) *snapshot = NULL;
  if (length) *length = 0;
  if (error) *error = NULL;
  if (!agent || !snapshot || !length)
    return fail(error, "invalid snapshot export arguments");
  cJSON *messages = motiris_messages(agent);
  if (validate_messages(messages, error)) return -1;
  digest = digest_messages(messages, session_id);
  if (!digest) return fail(error, "cannot digest snapshot");

  root = cJSON_CreateObject();
  copy = cJSON_Duplicate(messages, 1);
  if (!root || !copy) {
    fail(error, SAVE_ERROR);
    goto out;
  }
  if (!cJSON_AddStringToObject(root, "format", SNAPSHOT_FORMAT) ||
      !cJSON_AddNumberToObject(root, "version", SNAPSHOT_VERSION) ||
      !cJSON_AddStringToObject(root, "session_id", session_id ? session_id : "") ||
      !cJSON_AddItemToObject(root, "messages", copy)) {
    fail(error, SAVE_ERROR);
    goto out;
  }
  copy = NULL; /* root owns the message duplicate */
  if (!cJSON_AddStringToObject(root, "digest", digest)) {
    fail(error, SAVE_ERROR);
    goto out;
  }
  printed = cJSON_PrintUnformatted(root);
  if (!printed) { fail(error, SAVE_ERROR); goto out; }
  size_t used = strlen(printed);
  if (used > SNAPSHOT_MAX_BYTES) {
    fail(error, "snapshot byte budget exceeded");
    goto out;
  }
  *snapshot = printed;
  *length = used;
  printed = NULL;
  rc = 0;

out:
  free(printed);
  free(digest);
  cJSON_Delete(copy);
  cJSON_Delete(root);
  return rc;
}

int motiris_session_import(MotirisAgent *agent, const char *snapshot,
                           size_t length, const char *expected_session_id,
                           size_t *loaded_messages, const char **error) {
  static const char *const root_fields[] = {
    "format", "version", "session_id", "messages", "digest"
  };
  if (loaded_messages) *loaded_messages = 0;
  if (error) *error = NULL;
  if (!agent || !snapshot)
    return fail(error, "invalid snapshot import arguments");
  if (length > SNAPSHOT_MAX_BYTES)
    return fail(error, "snapshot byte budget exceeded");
  if (memchr(snapshot, '\0', length))
    return fail(error, "snapshot contains an embedded NUL");
  cJSON *target = motiris_messages(agent);
  if (!cJSON_IsArray(target) || target->child)
    return fail(error, "snapshot restore requires an empty context");

  const char *parse_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(snapshot, length + 1, &parse_end, 1);
  if (!root) return fail(error, "legacy or malformed snapshot rejected");
  if (!only_fields(root, root_fields,
                   sizeof root_fields / sizeof root_fields[0])) {
    cJSON_Delete(root);
    return fail(error, INVALID_STRUCTURE);
  }
  const char *format = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(root, "format"));
  cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
  const char *session_id = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(root, "session_id"));
  cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
  const char *digest = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(root, "digest"));
  if (!format || strcmp(format, SNAPSHOT_FORMAT) || !cJSON_IsNumber(version) ||
      version->valuedouble != SNAPSHOT_VERSION || !session_id || !digest) {
    cJSON_Delete(root);
    return fail(error, "unsupported snapshot version or header");
  }
  if (expected_session_id && strcmp(session_id, expected_session_id)) {
    cJSON_Delete(root);
    return fail(error, "session snapshot ownership mismatch");
  }
  if (validate_messages(messages, error)) { cJSON_Delete(root); return -1; }
  char *expected_digest = digest_messages(messages, session_id);
  if (!expected_digest) {
    cJSON_Delete(root);
    return fail(error, "cannot digest snapshot");
  }
  int digest_ok = !strcmp(digest, expected_digest);
  free(expected_digest);
  if (!digest_ok) {
    cJSON_Delete(root);
    return fail(error, "snapshot integrity check failed");
  }

  cJSON *copy = cJSON_Duplicate(messages, 1);
  if (!copy) { cJSON_Delete(root); return fail(error, "cannot restore snapshot"); }
  size_t count = 0;
  cJSON *message;
  while ((message = cJSON_DetachItemFromArray(copy, 0)) != NULL) {
    cJSON_AddItemToArray(target, message);
    count++;
  }
  cJSON_Delete(copy);
  cJSON_Delete(root);
  if (motiris_history_retain(agent)) {
    const char *message = motiris_last_error(agent);
    return fail(error, *message ? message : "history retention failed");
  }
  count = (size_t)cJSON_GetArraySize(target);
  if (loaded_messages) *loaded_messages = count;
  return 0;
}
