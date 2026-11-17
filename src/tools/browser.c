/* browser.c - headless browser fetch tool (issue #13).
 *
 * Runs a system headless Chromium (`chromium`/`chromium-browser`/
 * `google-chrome`) with --dump-dom and returns the rendered text.
 * The real browser does JS rendering, so this complements web_fetch
 * for client-rendered pages. Falls back gracefully when no browser
 * binary exists; MOTIRIS_BROWSER overrides the binary path.
 */
#include "motiris.h"
#include "childproc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BRO_CAP (64 * 1024)
#define BRO_DEFAULT_TIMEOUT_S 30L

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

static const char *find_browser(void) {
  const char *env = getenv("MOTIRIS_BROWSER");
  if (env && *env) return env;
  static const char *cands[] = {
    "chromium", "chromium-browser", "google-chrome",
    "google-chrome-stable", "headless_shell", NULL };
  for (int i = 0; cands[i]; i++)
    if (access(cands[i], X_OK) == 0) return cands[i];
  return NULL;
}

static void strip_html(char *s) {
  char *src = s, *dst = s;
  while (*src) {
    if (*src == '<') {
      char *gt = strchr(src, '>');
      if (!strncmp(src, "<script", 7) || !strncmp(src, "<style", 6)) {
        /* skip until matching close tag */
        char *end = strstr(src, src[1] == 's' ? "</script" : "</style");
        if (end) { src = strchr(end, '>'); if (src) src++; continue; }
      }
      if (gt) { src = gt + 1; continue; }
    }
    *dst++ = *src++;
  }
  *dst = '\0';
  char *w = s, *r = s;
  int prev_space = 0;
  while (*r) {
    if (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') {
      if (!prev_space) *w++ = ' ';
      prev_space = 1;
    } else { *w++ = *r; prev_space = 0; }
    r++;
  }
  *w = '\0';
}

static char *browser_fetch_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *url = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "url"));
  if (!url || !*url) { cJSON_Delete(args); return err_json("url required"); }
  /* copy url before args is freed (it points into the args tree) */
  char url_copy[1024];
  snprintf(url_copy, sizeof url_copy, "%s", url);

  const char *browser = find_browser();
  if (!browser) { cJSON_Delete(args); return err_json("no headless browser"); }

  char *const child_argv[] = {
    (char *)browser, "--headless", "--disable-gpu", "--no-sandbox",
    "--dump-dom", url_copy, NULL
  };
  ChildProcResult child;
  childproc_run("browser", child_argv, "MOTIRIS_BROWSER_TIMEOUT_S",
                BRO_DEFAULT_TIMEOUT_S, BRO_CAP, &child);
  cJSON_Delete(args);

  if (child.output) strip_html(child.output);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "url", url_copy);
  cJSON_AddStringToObject(r, "text", child.output ? child.output : "");
  cJSON_AddNumberToObject(r, "exit_code", child.exit_code);
  cJSON_AddNumberToObject(r, "signal", child.signal_number);
  cJSON_AddBoolToObject(r, "timed_out", child.timed_out);
  cJSON_AddBoolToObject(r, "truncated", child.truncated);
  if (child.error[0]) cJSON_AddStringToObject(r, "error", child.error);
  childproc_free(&child);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static const MotirisTool browser_tools[] = {
  { "browser_fetch",
    "Render a URL in a headless browser and return its text (JS runs). "
    "Requires chromium/chrome installed; MOTIRIS_BROWSER overrides.",
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"]}",
    browser_fetch_call, NULL },
};

void motiris_register_browser_tools(MotirisAgent *a) {
  for (size_t i = 0; i < sizeof browser_tools / sizeof browser_tools[0]; i++)
    motiris_register_tool(a, &browser_tools[i]);
}
