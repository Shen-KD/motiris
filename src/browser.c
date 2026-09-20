/* browser.c - headless browser fetch tool (issue #13).
 *
 * Runs a system headless Chromium (`chromium`/`chromium-browser`/
 * `google-chrome`) with --dump-dom and returns the rendered text.
 * The real browser does JS rendering, so this complements web_fetch
 * for client-rendered pages. Falls back gracefully when no browser
 * binary exists; MOTIRIS_BROWSER overrides the binary path.
 */
#include "motiris.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BRO_CAP (64 * 1024)

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

  int outpipe[2];
  if (pipe(outpipe)) { cJSON_Delete(args); return err_json("pipe failed"); }
  pid_t pid = fork();
  if (pid < 0) { cJSON_Delete(args); return err_json("fork failed"); }
  if (pid == 0) {
    dup2(outpipe[1], 1);
    dup2(outpipe[1], 2);
    close(outpipe[0]);
    close(outpipe[1]);
    execlp(browser, browser, "--headless", "--disable-gpu",
           "--no-sandbox", "--dump-dom", url, (char *)NULL);
    _exit(127);
  }
  close(outpipe[1]);
  size_t cap = 16384, len = 0;
  char *buf = malloc(cap);
  ssize_t n;
  while ((n = read(outpipe[0], buf + len, cap - len - 1)) > 0) {
    len += (size_t)n;
    if (len + 1 >= cap) {
      if (cap >= BRO_CAP) break;
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }
  close(outpipe[0]);
  buf[len] = '\0';
  int st;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}

  strip_html(buf);
  cJSON_Delete(args);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "url", url_copy);
  cJSON_AddStringToObject(r, "text", buf);
  free(buf);
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