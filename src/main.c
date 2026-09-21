/* main.c - motiris CLI: a single static-ish binary, no runtime deps.
 *   motiris -p "summarize this repo" -m deepseek-chat -b <url> -v
 *   echo "$(cat prompt.txt)" | motiris              (stdin as prompt)
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
"      --goal-check      confirm the goal is met (one extra round)\n"
"  -r, --resume FILE     resume previous session log\n"
"      --save FILE       persist session log to FILE\n"
"  -v, --verbose         print step/tool/trace to stderr\n"
"  -i, --interactive     force interactive REPL (default when tty)\n"
"      --stream          stream tokens as they arrive (once-run mode)\n"
"      --version         print version and exit\n"
"      --init            write $HOME/.motiris/{env,config.json} templates\n"
"      --sessions [TERM] list session logs (grep TERM if given)\n"
"      --cron [FILE]     run scheduled jobs (JSON array); --once = run\n"
"                        due jobs once and exit\n"
"      --gateway [LISTEN]  run as HTTP gateway (default :8899);\n"
"                          token via MOTIRIS_GATEWAY_TOKEN\n"
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

int motiris_repl(MotirisAgent *a);

int main(int argc, char **argv) {
  const char *model = NULL, *base_url = NULL, *key = NULL;
  const char *system = NULL, *prompt = NULL, *skill_dir = NULL;
  const char *resume = NULL, *save = NULL, *transport = NULL, *plugin_dir = NULL;
  const char *gateway_listen = NULL;
  int no_tools = 0, no_plugin = 0, verbose = 0, max_steps = 0;
  int interactive = 0, stream = 0, gateway_mode = 0, goal_check = 0;
  int cron_mode = 0, cron_once = 0;
  const char *cron_file = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define NEED() (i + 1 < argc ? argv[++i] : (usage(stderr), exit(2), ""))
    if (a[0] == '-' && a[1] && a[1] != '-') {
      /* short options: -m -b -k -s -S -p -r -v -i -h */
      switch (a[1]) {
        case 'm': model = NEED(); break;
        case 'b': base_url = NEED(); break;
        case 'k': key = NEED(); break;
        case 's': system = NEED(); break;
        case 'S': skill_dir = NEED(); break;
        case 'p': prompt = NEED(); break;
        case 'r': resume = NEED(); break;
        case 'v': verbose = 1; break;
        case 'i': interactive = 1; break;
        case 'h': usage(stdout); return 0;
        default:
          fprintf(stderr, "motiris: unknown option: %s\n", a);
          usage(stderr);
          return 2;
      }
      continue;
    }
    /* long options: --xxx, dispatch on third char then verify */
    switch (a[2]) {
      case 'm': /* --model | --max-steps */
        if (!strcmp(a, "--model")) { model = NEED(); break; }
        if (!strcmp(a, "--max-steps")) { max_steps = atoi(NEED()); break; }
        goto unknown;
      case 'b': /* --base-url */
        if (!strcmp(a, "--base-url")) { base_url = NEED(); break; }
        goto unknown;
      case 'a': /* --api-key */
        if (!strcmp(a, "--api-key")) { key = NEED(); break; }
        goto unknown;
      case 'g': /* --gateway | --goal-check */
        if (!strcmp(a, "--gateway")) {
          gateway_mode = 1;
          if (i + 1 < argc && argv[i + 1][0] != '-')
            gateway_listen = argv[++i];
          break;
        }
        if (!strcmp(a, "--goal-check")) { goal_check = 1; break; }
        goto unknown;
      case 's': /* --system | --save | --stream | --sessions | --skill-dir */
        if (!strcmp(a, "--system")) { system = NEED(); break; }
        if (!strcmp(a, "--save")) { save = NEED(); break; }
        if (!strcmp(a, "--stream")) { stream = 1; break; }
        if (!strcmp(a, "--sessions")) {
          const char *term = (i + 1 < argc && argv[i + 1][0] != '-')
              ? argv[++i] : NULL;
          return motiris_session_list("gateway", term) ? 1 : 0;
        }
        if (!strcmp(a, "--skill-dir")) { skill_dir = NEED(); break; }
        goto unknown;
      case 'p': /* --prompt | --plugin-dir */
        if (!strcmp(a, "--prompt")) { prompt = NEED(); break; }
        if (!strcmp(a, "--plugin-dir")) { plugin_dir = NEED(); break; }
        goto unknown;
      case 'r': /* --resume */
        if (!strcmp(a, "--resume")) { resume = NEED(); break; }
        goto unknown;
      case 't': /* --transport */
        if (!strcmp(a, "--transport")) { transport = NEED(); break; }
        goto unknown;
      case 'n': /* --no-tools | --no-plugin */
        if (!strcmp(a, "--no-tools")) { no_tools = 1; break; }
        if (!strcmp(a, "--no-plugin")) { no_plugin = 1; break; }
        goto unknown;
      case 'c': /* --cron */
        if (!strcmp(a, "--cron")) {
          cron_mode = 1;
          if (i + 1 < argc && argv[i + 1][0] != '-')
            cron_file = argv[++i];
          break;
        }
        goto unknown;
      case 'o': /* --once | --goal-check */
        if (!strcmp(a, "--once")) { cron_once = 1; break; }
        if (!strcmp(a, "--goal-check")) { goal_check = 1; break; }
        goto unknown;
      case 'i': /* --init | --interactive */
        if (!strcmp(a, "--init")) { return motiris_init_config() ? 2 : 0; }
        if (!strcmp(a, "--interactive")) { interactive = 1; break; }
        goto unknown;
      case 'v': /* --verbose | --version */
        if (!strcmp(a, "--verbose")) { verbose = 1; break; }
        if (!strcmp(a, "--version")) {
          printf("motiris %s (%s, built %s)\n", MOTIRIS_VERSION,
                 MOTIRIS_SHA, MOTIRIS_DT);
          return 0;
        }
        goto unknown;
      case 'h': /* --help */
        if (!strcmp(a, "--help")) { usage(stdout); return 0; }
        goto unknown;
      default:
      unknown:
        fprintf(stderr, "motiris: unknown option: %s\n", a);
        usage(stderr);
        return 2;
    }
#undef NEED
  }

  motiris_load_env();   /* $HOME/.motiris/env -> env defaults */

  if (gateway_mode) {
    const char *tok = getenv("MOTIRIS_GATEWAY_TOKEN");
    return motiris_gateway_run(gateway_listen, tok, !no_tools, verbose) ? 1 : 0;
  }

  if (cron_mode) {
    const char *file = cron_file;
    char def[512];
    if (!file) {
      const char *h = getenv("HOME");
      if (!h) h = ".";
      snprintf(def, sizeof def, "%s/.config/motiris/cron.json", h);
      file = def;
    }
    return motiris_cron_run(file, cron_once, verbose) ? 1 : 0;
  }

  MotirisAgent *ag = motiris_new();
  motiris_apply_config(ag);   /* config.json defaults (CLI flags win below) */
  if (model) motiris_set_model(ag, model);
  if (base_url) motiris_set_base_url(ag, base_url);
  if (key) motiris_set_api_key(ag, key);
  if (system) motiris_set_system(ag, system);
  if (transport) motiris_set_transport(ag, transport);
  if (max_steps > 0) motiris_set_max_steps(ag, max_steps);
  if (goal_check) motiris_set_goal_check(ag, 1);
  motiris_set_verbose(ag, verbose);
  if (stream) motiris_set_stream(ag, 1);
  if (no_tools) motiris_set_tools_enabled(ag, 0);
  if (no_plugin) motiris_set_plugins_enabled(ag, 0);
  if (plugin_dir) motiris_set_plugin_dir(ag, plugin_dir);
  motiris_tool_scan(plugin_dir);   /* index <dir>/<name>/<name>.json schemas first */
  motiris_register_core_tools(ag);
  if (motiris_tools_enabled(ag)) {
    motiris_register_file_tools(ag);
    motiris_register_web_tools(ag);
    motiris_register_memory_tools(ag);
    motiris_register_subagent_tools(ag);
    motiris_register_browser_tools(ag);
  }
  if (skill_dir) motiris_register_skill_tools(ag, skill_dir);
  if (motiris_plugins_enabled(ag)) motiris_load_plugins(ag, NULL);

  /* legacy --skill-dir injection kept only when tools are off */
  if (skill_dir && !motiris_tools_enabled(ag)) {
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
  else if (interactive || isatty(0)) {
    /* interactive REPL: multi-turn, streaming, history */
    int rc = motiris_repl(ag);
    motiris_free(ag);
    return rc;
  } else {
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