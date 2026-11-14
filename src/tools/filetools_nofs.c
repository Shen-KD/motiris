/* filetools_nofs.c - bounded stubs for builds without a filesystem backend. */
#include "motiris.h"

#include <stdlib.h>

static char *unavailable(const char *args_json, void *ud) {
  (void)args_json;
  (void)ud;
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error",
      "file tools unavailable: built without filesystem support");
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static const MotirisTool disabled_file_tools[] = {
  { "read_file", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}", unavailable, NULL },
  { "write_file", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
    unavailable, NULL },
  { "patch", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"old\"]}", unavailable, NULL },
  { "search_files", "Unavailable in this no-filesystem build.",
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
    unavailable, NULL },
};

void motiris_register_file_tools(MotirisAgent *a) {
  for (size_t i = 0;
       i < sizeof disabled_file_tools / sizeof disabled_file_tools[0]; i++)
    motiris_register_tool(a, &disabled_file_tools[i]);
}
