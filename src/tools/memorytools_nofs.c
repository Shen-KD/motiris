/* memorytools_nofs.c - fixed-error stubs for the optional host memory file backend. */
#include "motiris.h"

#include <stdlib.h>

static char *unavailable(const char *args_json, void *ud) {
  (void)args_json;
  (void)ud;
  cJSON *r = cJSON_CreateObject();
  if (!r) return NULL;
  cJSON_AddStringToObject(r, "error",
      "memory tools unavailable: built without filesystem support");
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static const MotirisTool disabled_memory_tools[] = {
  { "memory_set", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
    "\"value\":{\"type\":\"string\"}},\"required\":[\"key\",\"value\"]}",
    unavailable, NULL },
  { "memory_get", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}}}",
    unavailable, NULL },
};

void motiris_register_memory_tools(MotirisAgent *a) {
  for (size_t i = 0;
       i < sizeof disabled_memory_tools / sizeof disabled_memory_tools[0]; i++)
    motiris_register_tool(a, &disabled_memory_tools[i]);
}
