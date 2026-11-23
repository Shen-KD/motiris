/* context_compaction.c - deterministic, bounded old-context compactor.
 * No provider recursion: the newest complete turns and active user turn stay
 * intact, while older complete turns become one extractive summary exchange.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_TEXT_MAX 256

typedef struct {
  char *data;
  size_t length, limit;
  int truncated;
} Summary;

static void summary_bytes(Summary *summary, const char *text, size_t length) {
  if (summary->length >= summary->limit) {
    summary->truncated = 1;
    return;
  }
  size_t room = summary->limit - summary->length;
  if (length > room) {
    length = room;
    summary->truncated = 1;
  }
  memcpy(summary->data + summary->length, text, length);
  summary->length += length;
  summary->data[summary->length] = '\0';
}

static void summary_text(Summary *summary, const char *text) {
  if (text) summary_bytes(summary, text, strlen(text));
}

static void summary_content(Summary *summary, const char *text) {
  size_t emitted = 0;
  while (text && text[emitted] && emitted < RECORD_TEXT_MAX) {
    unsigned char c = (unsigned char)text[emitted++];
    char clean = (c < 32 || c == 127) ? ' ' : (char)c;
    summary_bytes(summary, &clean, 1);
  }
  if (text && text[emitted]) summary_text(summary, "..");
}

static const char *role_of(cJSON *message) {
  const char *role = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "role"));
  return role ? role : "unknown";
}

static cJSON *message_at(cJSON *messages, int index) {
  return cJSON_GetArrayItem(messages, index);
}

static int compaction_cut(cJSON *messages, size_t keep) {
  int count = cJSON_GetArraySize(messages);
  if (count < 0 || (size_t)count <= keep) return 0;
  int cut = count - (int)keep;
  while (cut > 0) {
    cJSON *message = message_at(messages, cut);
    if (message && !strcmp(role_of(message), "user")) break;
    cut--;
  }
  return cut;
}

static void utf8_clean(char *text) {
  unsigned char *s = (unsigned char *)text;
  for (size_t i = 0; s[i];) {
    unsigned char c = s[i];
    if (c < 128) { i++; continue; }
    size_t count = c >= 0xc2 && c <= 0xdf ? 2 :
                   c >= 0xe0 && c <= 0xef ? 3 :
                   c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    int valid = count != 0;
    for (size_t j = 1; valid && j < count; j++)
      valid = s[i + j] >= 0x80 && s[i + j] <= 0xbf;
    if (valid && count >= 3) {
      if (c == 0xe0) valid = s[i + 1] >= 0xa0;
      if (c == 0xed) valid = s[i + 1] <= 0x9f;
      if (c == 0xf0) valid = s[i + 1] >= 0x90;
      if (c == 0xf4) valid = s[i + 1] <= 0x8f;
    }
    if (!valid) { s[i] = '?'; i++; }
    else i += count;
  }
}

static char *build_summary(cJSON *messages, int cut, size_t max_chars) {
  Summary summary = { .limit = max_chars - 1 };
  summary.data = malloc(max_chars);
  if (!summary.data) return NULL;
  summary.data[0] = '\0';
  char heading[160];
  snprintf(heading, sizeof heading,
           "[Context compaction: %d older messages summarized; newest turns retained]\n",
           cut);
  summary_text(&summary, heading);
  for (int i = 0; i < cut && summary.length < summary.limit; i++) {
    cJSON *message = message_at(messages, i);
    const char *role = role_of(message);
    const char *content = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(message, "content"));
    summary_text(&summary, "- ");
    summary_text(&summary, role);
    summary_text(&summary, ": ");
    if (content) summary_content(&summary, content);
    else if (cJSON_GetObjectItemCaseSensitive(message, "tool_calls"))
      summary_text(&summary, "[assistant tool call]");
    else summary_text(&summary, "[no text]");
    summary_text(&summary, "\n");
  }
  if (summary.truncated) summary_text(&summary, "[summary truncated]\n");
  utf8_clean(summary.data);
  return summary.data;
}

static int compact_context(MotirisAgent *agent, size_t keep,
                           size_t max_chars, void *ud) {
  (void)ud;
  cJSON *messages = motiris_messages(agent);
  int cut = compaction_cut(messages, keep);
  if (!cut) return 0;
  char *text = build_summary(messages, cut, max_chars);
  if (!text) return -1;
  cJSON *summary = cJSON_CreateObject();
  cJSON *ack = cJSON_CreateObject();
  if (summary && ack) {
    cJSON_AddStringToObject(summary, "role", "user");
    cJSON_AddStringToObject(summary, "content", text);
    cJSON_AddStringToObject(ack, "role", "assistant");
    cJSON_AddStringToObject(ack, "content", "[compact summary accepted]");
  }
  free(text);
  if (!summary || !ack) {
    cJSON_Delete(summary);
    cJSON_Delete(ack);
    return -1;
  }

  /* All allocations succeeded. Array insertion only relinks nodes. */
  for (int i = 0; i < cut; i++) {
    cJSON *first = messages->child;
    cJSON_DetachItemViaPointer(messages, first);
    cJSON_Delete(first);
  }
  (void)cJSON_InsertItemInArray(messages, 0, ack);
  (void)cJSON_InsertItemInArray(messages, 0, summary);
  return 1;
}

int motiris_plugin_init(MotirisAgent *agent, const MotirisPluginApi *api) {
  if (!api || api->version < 2 || !api->set_compactor) return -1;
  api->set_compactor(agent, compact_context, NULL);
  return 0;
}
