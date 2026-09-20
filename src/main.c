/* main.c - motiris CLI: a single static-ish binary, no runtime deps.
 *   motiris -p "summarize this repo" -m deepseek-chat -b <url> -v
 *   echo "$(cat prompt.txt)" | motiris              (stdin as prompt)
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f) {
  fprintf(f,
"motiris - Iris as a mote: agent = model + harness, in one tiny binary.\n"
"\n"
"usage: motiris [options]\n"
"\n"
"  -m, --model NAME      model id (env MOTIRIS_MODEL, default gpt-4o-mini)\n"
"  -b, --base-url URL    chat completions endpoint\n"
"                        (default https://api.openai.com/v1/chat/completions)\n"
"  -k, --api-key KEY     bearer token (env MOTIRIS_API_KEY)\n"
"  -s, --system TEXT     system prompt\n"
"  -S, --skill-dir DIR   read *.md files here, inject as system prompt\n"
"  -p, --prompt TEXT     user prompt (default: read stdin)\n"
"      --no-tools        do not register built-in tools\n"
"      --transport NAME  curl (default) | echo (offline test)\n"
"      --max-steps N     agent loop bound (default 10)\n"
"  -r, --resume FILE     resume previous session log\n"
"      --save FILE       persist session log to FILE\n"
"  -v, --verbose         print step/tool/trace to stderr\n"
"  -h, --help            this help\n"
"\n"
"examples:\n"
"  motiris -p \"what time is it?\" -m deepseek-chat -k $DEEPSEEK_API_KEY\n"
"  printf 'list this dir' | motiris --transport echo          # offline demo\n");
}

static char *read_stdin(void) {
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  int c;
  while ((c = fgetc(stdin)) != EOF) {
    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    buf[len++] = (char)c;
  }
  buf[len] = '\0';
  return buf;
}

/* read all *.md files from a directory into one system prompt */
static char *load_skill_dir(const char *dir) {
  char cmd[1024];
  snprintf(cmd, sizeof cmd,
           "for f in %s/*.md; do [ -f \"$f\" ] && { echo \"# $f\"; "
           "cat \"$f\"; echo; }; done", dir);
  FILE *p = popen(cmd, "r");
  if (!p) return NULL;
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, p)) > 0) {
    len += n;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  buf[len] = '\0';
  pclose(p);
  return buf;
}

int main(int argc, char **argv) {
  const char *model = NULL, *base_url = NULL, *key = NULL;
  const char *system = NULL, *prompt = NULL, *skill_dir = NULL;
  const char *resume = NULL, *save = NULL, *transport = NULL, *plugin_dir = NULL;
  int no_tools = 0, no_plugin = 0, verbose = 0, max_steps = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define NEED() (i + 1 < argc ? argv[++i] : (usage(stderr), exit(2), ""))
    if      (!strcmp(a, "-m") || !strcmp(a, "--model")) model = NEED();
    else if (!strcmp(a, "-b") || !strcmp(a, "--base-url")) base_url = NEED();
    else if (!strcmp(a, "-k") || !strcmp(a, "--api-key")) key = NEED();
    else if (!strcmp(a, "-s") || !strcmp(a, "--system")) system = NEED();
    else if (!strcmp(a, "-S") || !strcmp(a, "--skill-dir")) skill_dir = NEED();
    else if (!strcmp(a, "-p") || !strcmp(a, "--prompt")) prompt = NEED();
    else if (!strcmp(a, "-r") || !strcmp(a, "--resume")) resume = NEED();
    else if (!strcmp(a, "--save")) save = NEED();
    else if (!strcmp(a, "--transport")) transport = NEED();
    else if (!strcmp(a, "--max-steps")) max_steps = atoi(NEED());
    else if (!strcmp(a, "--no-tools")) no_tools = 1;
    else if (!strcmp(a, "--plugin-dir")) plugin_dir = NEED();
    else if (!strcmp(a, "--no-plugin")) no_plugin = 1;
    else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) verbose = 1;
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    else { fprintf(stderr, "motiris: unknown option: %s\n", a); usage(stderr); return 2; }
#undef NEED
  }

  MotirisAgent *ag = motiris_new();
  if (model) motiris_set_model(ag, model);
  if (base_url) motiris_set_base_url(ag, base_url);
  if (key) motiris_set_api_key(ag, key);
  if (system) motiris_set_system(ag, system);
  if (transport) motiris_set_transport(ag, transport);
  if (max_steps > 0) motiris_set_max_steps(ag, max_steps);
  motiris_set_verbose(ag, verbose);
  if (!no_tools) motiris_register_core_tools(ag);
  if (!no_plugin) motiris_load_plugins(ag, plugin_dir);

  /* skills: injected as system context (kept in front of user prompt) */
  if (skill_dir) {
    char *sk = load_skill_dir(skill_dir);
    if (sk && *sk) {
      if (system) {
        size_t n = strlen(system) + strlen(sk) + 4;
        char *both = malloc(n);
        snprintf(both, n, "%s\n\n%s", system, sk);
        motiris_set_system(ag, both);
        free(both);
      } else motiris_set_system(ag, sk);
    }
    free(sk);
  }

  /* resume: replay a saved session log line by line */
  if (resume) {
    FILE *f = fopen(resume, "r");
    if (!f) { fprintf(stderr, "motiris: cannot open %s\n", resume); return 2; }
    char line[65536];
    while (fgets(line, sizeof line, f))
      if (line[0] == 'U') motiris_add_user(ag, line + 1);
    fclose(f);
  }

  if (prompt) motiris_add_user(ag, prompt);
  else {
    char *in = read_stdin();
    if (*in) motiris_add_user(ag, in);
    free(in);
  }

  cJSON *msgs = motiris_messages(ag);
  if (!msgs || !msgs->child) {
    fprintf(stderr, "motiris: no prompt (use -p or pipe stdin)\n");
    motiris_free(ag);
    return 2;
  }

  int rc = motiris_run(ag);
  if (rc) fprintf(stderr, "motiris: %s\n", motiris_last_error(ag));
  if (save) {
    /* append-only trace: U=user, A=assistant content, T=tool result.
       resume (-r) replays U lines; the rest stay for human inspection. */
    FILE *f = fopen(save, "a");
    if (f) {
      for (cJSON *m = motiris_messages(ag)->child; m; m = m->next) {
        const char *role = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(m, "role"));
        const char *content = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(m, "content"));
        if (!role || !role[0]) continue;
        char tag;
        switch (role[0]) {
          case 'u': tag = 'U'; break;
          case 'a': tag = 'A'; break;
          case 't': tag = 'T'; break;
          default: continue; /* system etc. */
        }
        fprintf(f, "%c%s\n", tag, content ? content : "");
      }
      fclose(f);
    }
  }
  motiris_free(ag);
  return rc;
}