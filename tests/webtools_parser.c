/* Direct regression tests for web_search's production DDG response parser. */
#include "../src/tools/webtools.c"

#include <stdio.h>

/* Only parser tests are linked; registration itself is not under test. */
void motiris_register_tool(MotirisAgent *a, const MotirisTool *t) {
  (void)a;
  (void)t;
}

static void fail(const char *what, const char *input) {
  fprintf(stderr, "webtools parser: %s\ninput: %.160s\n", what, input);
  exit(1);
}

/* Use an exact heap allocation so ASAN can detect reads beyond the response. */
static cJSON *parse_n(const char *html, size_t len, int *count) {
  char *owned = malloc(len + 1);
  if (!owned) exit(2);
  memcpy(owned, html, len);
  owned[len] = '\0';

  cJSON *out = cJSON_CreateArray();
  if (!out) exit(2);
  *count = 0;
  parse_ddg(owned, out, count);
  free(owned);
  if (*count != cJSON_GetArraySize(out))
    fail("count does not match result array", html);
  return out;
}

static void expect_invalid(const char *html) {
  int count;
  cJSON *out = parse_n(html, strlen(html), &count);
  if (count != 0) fail("malformed candidate was not skipped", html);
  cJSON_Delete(out);
}

static void expect_one(const char *html, const char *url, const char *title) {
  int count;
  cJSON *out = parse_n(html, strlen(html), &count);
  if (count != 1) fail("expected exactly one valid result", html);
  cJSON *item = cJSON_GetArrayItem(out, 0);
  const char *actual_url = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(item, "url"));
  const char *actual_title = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(item, "title"));
  if (!actual_url || strcmp(actual_url, url) != 0)
    fail("URL mismatch", html);
  if (!actual_title || strcmp(actual_title, title) != 0)
    fail("title mismatch", html);
  cJSON_Delete(out);
}

static void test_malformed(void) {
  static const char *const bad[] = {
    "rel=\"nofollow\"",
    "<a rel=\"nofollow\" href=",
    "<a rel=\"nofollow\" href=\"unterminated",
    "<a rel=\"nofollow\" href=\"https://example.test\"",
    "<a rel=\"nofollow\" href=\"https://example.test\">title",
    "<a rel=\"nofollow\" href=\"https://example.test\">title</a",
    "<a rel=\"nofollow\" other=\"unterminated",
  };
  static const char valid[] =
      "<a rel=\"nofollow\" href=\"https://example.test/recovered\">"
      "Recovered result</a>";
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    expect_invalid(bad[i]);
    char combined[512];
    int n = snprintf(combined, sizeof combined, "%s%s", bad[i], valid);
    if (n < 0 || (size_t)n >= sizeof combined) exit(2);
    expect_one(combined, "https://example.test/recovered", "Recovered result");
  }
}

static void test_every_truncation(void) {
  static const char valid[] =
      "<a rel=\"nofollow\" href=\"https://example.test/result\">"
      "Complete title</a>";
  static const char recovered[] =
      "<a rel=\"nofollow\" href=\"https://example.test/recovered\">"
      "Recovered result</a>";
  const size_t len = strlen(valid);
  for (size_t n = 0; n <= len; n++) {
    int count;
    cJSON *out = parse_n(valid, n, &count);
    int expected = n == len ? 1 : 0;
    if (count != expected) fail("unexpected result for truncated prefix", valid);
    cJSON_Delete(out);

    if (n < len) {
      char combined[512];
      int written = snprintf(combined, sizeof combined, "%.*s%s",
                             (int)n, valid, recovered);
      if (written < 0 || (size_t)written >= sizeof combined) exit(2);
      expect_one(combined, "https://example.test/recovered",
                 "Recovered result");
    }
  }
}

static void test_many_malformed(void) {
  char html[65536];
  size_t used = 0;
  for (int i = 0; i < 1000; i++) {
    int n = snprintf(html + used, sizeof html - used,
                     "<a rel=\"nofollow\">");
    if (n < 0 || (size_t)n >= sizeof html - used) exit(2);
    used += (size_t)n;
  }
  int n = snprintf(html + used, sizeof html - used,
      "<a rel=\"nofollow\" href=\"https://example.test/last\">Last</a>");
  if (n < 0 || (size_t)n >= sizeof html - used) exit(2);
  expect_one(html, "https://example.test/last", "Last");
}

static void test_long_fields(void) {
  static const size_t lengths[] = {511, 512, 513, 4096};
  char title[241];
  memset(title, 'T', 240);
  title[240] = '\0';

  for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
    size_t n = lengths[i];
    size_t html_len = n + sizeof title + 128;
    char *html = malloc(html_len);
    char *url = malloc(n + 1);
    if (!html || !url) exit(2);
    memset(url, 'u', n);
    url[n] = '\0';
    int written = snprintf(html, html_len,
        "<a rel=\"nofollow\" href=\"%s\">%s</a>", url, title);
    if (written < 0 || (size_t)written >= html_len) exit(2);

    size_t kept_url = n < 511 ? n : 511;
    url[kept_url] = '\0';
    title[200] = '\0';
    expect_one(html, url, title);
    title[200] = 'T';
    free(url);
    free(html);
  }
}

static void test_result_cap(void) {
  char html[4096];
  size_t used = 0;
  for (int i = 0; i < 12; i++) {
    int n = snprintf(html + used, sizeof html - used,
        "<a rel=\"nofollow\" href=\"https://example.test/%d\">r%d</a>", i, i);
    if (n < 0 || (size_t)n >= sizeof html - used) exit(2);
    used += (size_t)n;
  }
  int count;
  cJSON *out = parse_n(html, used, &count);
  if (count != 10) fail("ten-result cap changed", html);
  cJSON_Delete(out);
}

static unsigned fuzz_state = 0x50da7a11u;

static unsigned fuzz_next(void) {
  unsigned x = fuzz_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  fuzz_state = x;
  return x;
}

static void test_random_malformed(void) {
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      " <>/=\".-_:\t\n";
  char input[257];
  for (int i = 0; i < 1000; i++) {
    size_t len = fuzz_next() % sizeof input;
    for (size_t j = 0; j < len; j++)
      input[j] = alphabet[fuzz_next() % (sizeof alphabet - 1)];
    input[len] = '\0';
    int count;
    cJSON *out = parse_n(input, len, &count);
    if (count < 0 || count > 10) fail("random result count escaped cap", input);
    cJSON_Delete(out);
  }
}

int main(void) {
  test_malformed();
  test_every_truncation();
  expect_one("<a rel=\"nofollow\" href=\"https://example.test/ok\">"
             "Normal result</a>", "https://example.test/ok", "Normal result");
  expect_one("<a rel=\"nofollow\" href=\"https://example.test/marker\">"
             "Literal rel=\"nofollow\" text</a>",
             "https://example.test/marker", "Literal rel=\"nofollow\" text");
  test_many_malformed();
  test_long_fields();
  test_result_cap();
  test_random_malformed();
  puts("webtools parser: malformed, truncation, fuzz, boundary and normal rows ok");
  return 0;
}
