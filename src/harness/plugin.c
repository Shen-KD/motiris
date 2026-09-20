/* plugin.c - runtime plugin loader.
 *
 * Plugins are shared objects (.so) dropped into a plugins directory.
 * At startup motiris dlopens each one, checks the exported
 * `motiris_plugin_init` symbol and calls it with the plugin API, which is
 * how a plugin registers its tools with the running agent. Tools can be
 * unplugged again at runtime via motiris_unregister_tool.
 *
 * Layout of a plugin file (see examples/hello_plugin.c):
 *   int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api) {
 *     MotirisTool t = {...}; return api->register_tool(a, &t); }
 *
 * Directory resolution order (first existing one wins):
 *   1. MOTIRIS_PLUGIN_DIR env
 *   2. $HOME/.local/share/motiris/plugins
 *   3. ./plugins (cwd)
 * A missing directory is not an error; a broken .so inside is skipped
 * with a warning to stderr.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>

static const MotirisPluginApi api = {
  .version = MOTIRIS_PLUGIN_API_VERSION,
  .register_tool = motiris_register_tool,
  .unregister_tool = motiris_unregister_tool,
  .add_tool_hook = motiris_add_tool_hook,
};

static char *resolve_dir(const char *dir) {
  if (dir && *dir) return strdup(dir);
  const char *env = getenv("MOTIRIS_PLUGIN_DIR");
  if (env && *env) return strdup(env);
  const char *home = getenv("HOME");
  if (home) {
    size_t n = strlen(home) + 40;
    char *p = malloc(n);
    snprintf(p, n, "%s/.local/share/motiris/plugins", home);
    return p;
  }
  return strdup("./plugins");
}

static void load_one(MotirisAgent *a, const char *path) {
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    fprintf(stderr, "motiris: plugin %s: %s\n", path, dlerror());
    return;
  }
  motiris_plugin_init_fn init;
  union { void *p; motiris_plugin_init_fn f; } u;
  u.p = dlsym(h, "motiris_plugin_init");
  init = u.f;
  if (!init) {
    fprintf(stderr, "motiris: plugin %s: no motiris_plugin_init symbol\n", path);
    dlerror(); /* clear */
    dlclose(h);
    return;
  }
  if (init(a, &api) != 0)
    fprintf(stderr, "motiris: plugin %s: init returned error\n", path);
}

int motiris_load_plugins(MotirisAgent *a, const char *dir) {
  if (motiris_plugin_dir(a) && *motiris_plugin_dir(a)) dir = motiris_plugin_dir(a);
  char *d = resolve_dir(dir);

  DIR *dh = opendir(d);
  if (!dh) { free(d); return 0; } /* absent dir is fine */

  struct dirent *e;
  while ((e = readdir(dh)) != NULL) {
    size_t len = strlen(e->d_name);
    if (len < 4 || strcmp(e->d_name + len - 3, ".so")) continue;
    size_t n = strlen(d) + len + 2;
    char *path = malloc(n);
    snprintf(path, n, "%s/%s", d, e->d_name);
    load_one(a, path);
    dlerror(); /* clear any pending error before next dlsym */
    free(path);
  }
  closedir(dh);
  free(d);
  return 0;
}