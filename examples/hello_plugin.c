/* hello_plugin.c - example runtime plugin for motiris.
 *
 * Build:  cc -fPIC -shared -I../include -I../src/vendor -o hello_plugin.so \
 *             hello_plugin.c
 * Usage:  mkdir -p /tmp/motiris-plugins && cp hello_plugin.so /tmp/motiris-plugins/
 *         ./motiris -p "say hello" --plugin-dir /tmp/motiris-plugins
 *
 * A plugin is a shared object exporting one symbol:
 *   int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api);
 * It registers tools (and could later register hooks/commands) through
 * the api struct. Uses only cJSON + motiris.h - plugins stay tiny.
 */
#include "motiris.h"

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

int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api) {
  MotirisTool t = {
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