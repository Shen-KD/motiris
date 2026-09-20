/* hello_plugin.c - example runtime plugin for mote.
 *
 * Build:  cc -fPIC -shared -I../include -I../src/vendor -o hello_plugin.so \
 *             hello_plugin.c
 * Usage:  mkdir -p /tmp/mote-plugins && cp hello_plugin.so /tmp/mote-plugins/
 *         ./mote -p "say hello" --plugin-dir /tmp/mote-plugins
 *
 * A plugin is a shared object exporting one symbol:
 *   int mote_plugin_init(MoteAgent *a, const MotePluginApi *api);
 * It registers tools (and could later register hooks/commands) through
 * the api struct. Uses only cJSON + mote.h - plugins stay tiny.
 */
#include "mote.h"

#include <stdio.h>
#include <string.h>

static char *hello_call(const char *args_json, void *ud) {
  (void)ud;
  const char *name = "world";
  cJSON *args = cJSON_Parse(args_json);
  if (args) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (cJSON_IsString(n) && n->valuestring) name = n->valuestring;
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "greeting", name);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  cJSON_Delete(args);
  return s;
}

int mote_plugin_init(MoteAgent *a, const MotePluginApi *api) {
  MoteTool t = {
    .name = "hello",
    .description = "Return a friendly greeting for a given name.",
    .parameters = "{\"type\":\"object\",\"properties\":{\"name\":{"
                  "\"type\":\"string\"}}}",
    .call = hello_call,
    .ud = NULL,
  };
  api->register_tool(a, &t);
  fprintf(stderr, "hello plugin: tool 'hello' registered\n");
  return 0;
}