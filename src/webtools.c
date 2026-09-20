/* webtools.c - web_fetch / web_search tools (issue #5).
 *
 * web_fetch: GET a URL via libcurl, strip HTML tags/scripts, return
 * plain text (capped). web_search: query DuckDuckGo lite (no API key)
 * and return result titles/links/text snippets. Both stay JSON-shaped
 * for the model and are offline-testable against the mock server.
 */
#include "motiris.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FETCH_CAP (64 * 1024)

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

/* strip <script>..</script>, <style>..</style> and remaining tags */
static void strip_html(char *s) {
  char *src = s, *dst = s;
  int skip_deep = 0;
  while (*src) {
    if (!skip_deep && !strncmp(src, "<script", 7)) { skip_deep = 4; }
    if (!skip_deep && !strncmp(src, "<style", 6)) skip_deep = 4;
    if (*src == '<') {
      char *gt = strchr(src, '>');
      const char *low = src + 1;
      if (skip_deep) {
        if (!strncmp(src + 1, "/script", 7) || !strncmp(src + 1, "/style", 6))
          skip_deep = 0;
        if (gt) src = gt + 1;
        else break;
        continue;
      }
      if (gt && (*low == '/' || *low == '!' ||
                 (*low >= 'a' && *low <= 'z') ||
                 (*low >= 'A' && *low <= 'Z'))) {
        src = gt + 1;
        continue;
      }
    }
    *dst++ = *src++;
  }
  *dst = '\0';
  /* collapse whitespace runs */
  char *w = s, *r = s;
  int prev_space = 0;
  while (*r) {
    if (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') {
      if (!prev_space) *w++ = ' ';
      prev_space = 1;
    } else {
      *w++ = *r;
      prev_space = 0;
    }
    r++;
  }
  *w = '\0';
  /* trim leading space */
  char *t = s;
  while (*t == ' ') t++;
  if (t != s) memmove(s, t, strlen(t) + 1);
}

typedef struct { char *data; size_t len; size_t cap; } WBuf;

static size_t wbuf_cb(char *p, size_t sz, size_t nm, void *ud) {
  WBuf *b = ud;
  size_t n = sz * nm;
  if (b->len + n + 1 > FETCH_CAP) n = FETCH_CAP - b->len - 1;
  if (b->len + n + 1 > b->cap) {
    b->cap = b->cap ? b->cap * 2 : 16384;
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
  return n;
}

static char *get_url(const char *url, char *errb, size_t errn) {
  CURL *c = curl_easy_init();
  if (!c) { snprintf(errb, errn, "curl init failed"); return NULL; }

  WBuf b = {0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "motiris/0.1");
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wbuf_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);

  CURLcode rc = curl_easy_perform(c);
  long http = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_easy_cleanup(c);

  if (rc != CURLE_OK) {
    snprintf(errb, errn, "curl: %s", curl_easy_strerror(rc));
    free(b.data);
    return NULL;
  }
  if (http != 200) {
    snprintf(errb, errn, "http status %ld", http);
    free(b.data);
    return NULL;
  }
  return b.data;
}

/* ---------------- web_fetch ---------------- */
static char *web_fetch_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *url = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "url"));
  if (!url || !*url) { cJSON_Delete(args); return err_json("url required"); }

  char errb[256];
  char *raw = get_url(url, errb, sizeof errb);
  if (!raw) { cJSON_Delete(args); return err_json(errb); }

  strip_html(raw);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "url", url);
  cJSON_AddStringToObject(r, "text", raw);
  free(raw);
  cJSON_Delete(args);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- web_search (DuckDuckGo lite, keyless) ---------------- */
static const char *DDG = "https://lite.duckduckgo.com/lite/?q=";

static void urlencode(char *out, size_t outn, const char *in) {
  static const char hex[] = "0123456789ABCDEF";
  size_t j = 0;
  for (const unsigned char *c = (const unsigned char *)in;
       *c && j + 4 < outn; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
        (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.') {
      out[j++] = (char)*c;
    } else {
      out[j++] = '%';
      out[j++] = hex[*c >> 4];
      out[j++] = hex[*c & 15];
    }
  }
  out[j] = '\0';
}

/* crude but effective: grab <a rel="nofollow" href="...">title</a> rows */
static void parse_ddg(const char *html, cJSON *out, int *count) {
  const char *p = html;
  while (*p && *count < 10) {
    p = strstr(p, "rel=\"nofollow\"");
    if (!p) break;
    const char *href = strchr(p, '"');
    href = href ? strchr(href + 1, '"') : NULL;
    if (href) {
      char link[512];
      size_t n = (size_t)(strchr(href + 1, '"') - href - 1);
      if (n >= sizeof link) n = sizeof link - 1;
      memcpy(link, href + 1, n);
      link[n] = '\0';
      const char *t = href + n + 2;
      const char *te = strstr(t, "</a>");
      if (te) {
        size_t tl = (size_t)(te - t);
        if (tl > 200) tl = 200;
        char title[256];
        memcpy(title, t, tl);
        title[tl] = '\0';
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "title", title);
        cJSON_AddStringToObject(m, "url", link);
        cJSON_AddItemToArray(out, m);
        (*count)++;
        p = te;
      }
    }
    p = p ? p + 1 : NULL;
  }
}

static char *web_search_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *q = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "query"));
  if (!q || !*q) { cJSON_Delete(args); return err_json("query required"); }

  char enc[512];
  urlencode(enc, sizeof enc, q);
  char url[1024];
  snprintf(url, sizeof url, "%s%s", DDG, enc);

  char errb[256];
  char *html = get_url(url, errb, sizeof errb);
  cJSON_Delete(args);
  if (!html) return err_json(errb);

  cJSON *results = cJSON_CreateArray();
  int count = 0;
  parse_ddg(html, results, &count);
  free(html);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "results", results);
  cJSON_AddNumberToObject(r, "count", (double)count);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- registration ---------------- */
static const MotirisTool web_tools[] = {
  { "web_fetch",
    "Fetch a URL and return its text content (HTML stripped).",
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"]}",
    web_fetch_call, NULL },
  { "web_search",
    "Search the web (DuckDuckGo, no key) and return result titles+links.",
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},"
    "\"required\":[\"query\"]}",
    web_search_call, NULL },
};

void motiris_register_web_tools(MotirisAgent *a) {
  for (size_t i = 0; i < sizeof web_tools / sizeof web_tools[0]; i++)
    motiris_register_tool(a, &web_tools[i]);
}