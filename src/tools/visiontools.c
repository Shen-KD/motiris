/* visiontools.c - optional host-backed image description tool (issue #29).
 *
 * Build with VISION_TOOLS=1. One workspace-local image is encoded as a data
 * URL and sent to an OpenAI-compatible vision endpoint. Inference stays on
 * the configured provider; this module contains no image model or media
 * pipeline. Input, request, response, output and I/O time are bounded.
 */
#include "motiris.h"

#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VISION_DEFAULT_MAX_BYTES (256 * 1024)
#define VISION_HARD_MAX_BYTES (1024 * 1024)
#define VISION_RESPONSE_CAP (64 * 1024)
#define VISION_DESCRIPTION_CAP (16 * 1024)
#define VISION_QUESTION_CAP 4096
#define VISION_DEFAULT_TIMEOUT_S 30L
#define VISION_MAX_TIMEOUT_S 120L

static char *err_json(const char *msg) {
  cJSON *e = cJSON_CreateObject();
  if (!e) return NULL;
  cJSON_AddStringToObject(e, "error", msg);
  char *s = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  return s;
}

static char *errf_json(const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  return err_json(msg);
}

static int env_long(const char *name, long dflt, long max, long *out) {
  const char *s = getenv(name);
  if (!s || !*s) { *out = dflt; return 0; }
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || end == s || !end || *end || v < 1 || v > max) return -1;
  *out = v;
  return 0;
}

/* Resolve both paths before opening, so absolute paths and symlink targets
 * cannot escape the workspace. The final open also uses O_NOFOLLOW. */
static char *resolve_image(const char *path, char *errb, size_t errn) {
  if (!path || !*path) {
    snprintf(errb, errn, "image path required");
    return NULL;
  }
  if (strlen(path) >= PATH_MAX) {
    snprintf(errb, errn, "image path too long");
    return NULL;
  }
  char wd[PATH_MAX];
  const char *ws = getenv("MOTIRIS_WORKSPACE");
  if (!ws || !*ws) {
    if (!getcwd(wd, sizeof wd)) {
      snprintf(errb, errn, "workspace unavailable");
      return NULL;
    }
    ws = wd;
  }
  char candidate[PATH_MAX];
  int n = path[0] == '/'
      ? snprintf(candidate, sizeof candidate, "%s", path)
      : snprintf(candidate, sizeof candidate, "%s/%s", ws, path);
  if (n < 0 || (size_t)n >= sizeof candidate) {
    snprintf(errb, errn, "image path too long");
    return NULL;
  }
  char *resolved = realpath(candidate, NULL);
  char *root = realpath(ws, NULL);
  if (!resolved || !root) {
    snprintf(errb, errn, "cannot resolve image or workspace");
    free(resolved);
    free(root);
    return NULL;
  }
  size_t rn = strlen(root);
  int inside = !strcmp(root, "/")
      ? resolved[0] == '/'
      : !strncmp(resolved, root, rn) &&
        (resolved[rn] == '\0' || resolved[rn] == '/');
  free(root);
  if (!inside) {
    snprintf(errb, errn, "image path outside workspace");
    free(resolved);
    return NULL;
  }
  return resolved;
}

static int read_image(const char *path, size_t limit, unsigned char **out,
                      size_t *out_len, char *errb, size_t errn) {
  char *resolved = resolve_image(path, errb, errn);
  if (!resolved) return -1;
  int fd = open(resolved, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  free(resolved);
  if (fd < 0) { snprintf(errb, errn, "cannot open image"); return -1; }

  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    close(fd);
    snprintf(errb, errn, "image must be a non-empty regular file");
    return -1;
  }
  if ((uintmax_t)st.st_size > (uintmax_t)limit) {
    close(fd);
    snprintf(errb, errn, "image exceeds configured %zu-byte limit", limit);
    return -1;
  }
  size_t n = (size_t)st.st_size;
  unsigned char *buf = malloc(n);
  if (!buf) {
    close(fd);
    snprintf(errb, errn, "image allocation failed");
    return -1;
  }
  size_t off = 0;
  while (off < n) {
    ssize_t got = read(fd, buf + off, n - off);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      free(buf);
      close(fd);
      snprintf(errb, errn, "image changed or could not be read");
      return -1;
    }
    off += (size_t)got;
  }
  unsigned char extra;
  ssize_t got;
  do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
  close(fd);
  if (got != 0) {
    free(buf);
    snprintf(errb, errn, "image changed or exceeds configured limit");
    return -1;
  }
  *out = buf;
  *out_len = n;
  return 0;
}

static char *image_data_url(const unsigned char *data, size_t n,
                            const char *mime) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t encoded = 4 * ((n + 2) / 3);
  char prefix[80];
  int pn = snprintf(prefix, sizeof prefix, "data:%s;base64,", mime);
  if (pn < 0 || (size_t)pn >= sizeof prefix) return NULL;
  size_t p = (size_t)pn;
  char *url = malloc(p + encoded + 1);
  if (!url) return NULL;
  memcpy(url, prefix, p);

  size_t i = 0, o = p;
  while (i + 3 <= n) {
    uint32_t v = ((uint32_t)data[i] << 16) |
                 ((uint32_t)data[i + 1] << 8) | data[i + 2];
    url[o++] = table[(v >> 18) & 63];
    url[o++] = table[(v >> 12) & 63];
    url[o++] = table[(v >> 6) & 63];
    url[o++] = table[v & 63];
    i += 3;
  }
  if (i < n) {
    uint32_t v = (uint32_t)data[i] << 16;
    url[o++] = table[(v >> 18) & 63];
    if (i + 1 < n) {
      v |= (uint32_t)data[i + 1] << 8;
      url[o++] = table[(v >> 12) & 63];
      url[o++] = table[(v >> 6) & 63];
      url[o++] = '=';
    } else {
      url[o++] = table[(v >> 12) & 63];
      url[o++] = '=';
      url[o++] = '=';
    }
  }
  url[o] = '\0';
  return url;
}

static char *build_vision_request(const char *model, const char *question,
                                  const char *data_url) {
  cJSON *req = cJSON_CreateObject();
  if (!req || !cJSON_AddStringToObject(req, "model", model)) goto fail;
  cJSON *messages = cJSON_CreateArray();
  if (!messages) goto fail;
  cJSON_AddItemToObject(req, "messages", messages);
  cJSON *message = cJSON_CreateObject();
  if (!message) goto fail;
  cJSON_AddItemToArray(messages, message);
  if (!cJSON_AddStringToObject(message, "role", "user")) goto fail;
  cJSON *content = cJSON_CreateArray();
  if (!content) goto fail;
  cJSON_AddItemToObject(message, "content", content);

  cJSON *text = cJSON_CreateObject();
  if (!text) goto fail;
  cJSON_AddItemToArray(content, text);
  if (!cJSON_AddStringToObject(text, "type", "text") ||
      !cJSON_AddStringToObject(text, "text", question)) goto fail;

  cJSON *image = cJSON_CreateObject();
  if (!image) goto fail;
  cJSON_AddItemToArray(content, image);
  if (!cJSON_AddStringToObject(image, "type", "image_url")) goto fail;
  cJSON *url = cJSON_CreateObject();
  if (!url) goto fail;
  cJSON_AddItemToObject(image, "image_url", url);
  cJSON *ref = cJSON_CreateStringReference(data_url);
  if (!ref) goto fail;
  cJSON_AddItemToObject(url, "url", ref);

  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  return body;
fail:
  cJSON_Delete(req);
  return NULL;
}

typedef struct {
  char *data;
  size_t len, cap;
  int overflow, oom;
} VisionResponse;

static size_t response_write(char *ptr, size_t size, size_t nmemb, void *ud) {
  VisionResponse *r = ud;
  if (size && nmemb > SIZE_MAX / size) { r->overflow = 1; return 0; }
  size_t n = size * nmemb;
  if (n > VISION_RESPONSE_CAP - r->len) { r->overflow = 1; return 0; }
  size_t need = r->len + n + 1;
  if (need > r->cap) {
    size_t cap = r->cap ? r->cap : 4096;
    while (cap < need) {
      size_t next = cap * 2;
      cap = next > VISION_RESPONSE_CAP + 1 ? VISION_RESPONSE_CAP + 1 : next;
    }
    char *tmp = realloc(r->data, cap);
    if (!tmp) { r->oom = 1; return 0; }
    r->data = tmp;
    r->cap = cap;
  }
  memcpy(r->data + r->len, ptr, n);
  r->len += n;
  r->data[r->len] = '\0';
  return n;
}

static char *post_vision(const char *url, const char *key, const char *body,
                         long timeout, long *status, char *errb, size_t errn) {
  CURL *curl = curl_easy_init();
  if (!curl) { snprintf(errb, errn, "vision curl init failed"); return NULL; }
  struct curl_slist *headers = curl_slist_append(
      NULL, "Content-Type: application/json");
  if (headers) {
    struct curl_slist *next = curl_slist_append(headers,
                                                "Accept: application/json");
    if (next) headers = next;
    else {
      curl_slist_free_all(headers);
      headers = NULL;
    }
  }
  if (!headers) {
    curl_easy_cleanup(curl);
    snprintf(errb, errn, "vision header allocation failed");
    return NULL;
  }
  char auth[2304];
  if (key && *key) {
    if (strlen(key) > 2048) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      snprintf(errb, errn, "vision API key too long");
      return NULL;
    }
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
    struct curl_slist *with_auth = curl_slist_append(headers, auth);
    if (!with_auth) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      snprintf(errb, errn, "vision header allocation failed");
      return NULL;
    }
    headers = with_auth;
  }

  VisionResponse response = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout < 10 ? timeout : 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "motiris-vision/0.1");

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    if (response.overflow)
      snprintf(errb, errn, "vision provider response exceeds 64KiB");
    else if (response.oom)
      snprintf(errb, errn, "vision response allocation failed");
    else
      snprintf(errb, errn, "vision request failed: %s", curl_easy_strerror(rc));
    free(response.data);
    return NULL;
  }
  if (!response.data) {
    response.data = malloc(1);
    if (response.data) response.data[0] = '\0';
    else {
      snprintf(errb, errn, "vision response allocation failed");
      return NULL;
    }
  }
  return response.data;
}

static void provider_error_copy(const char *response, char *out, size_t outn) {
  cJSON *j = cJSON_Parse(response);
  const char *message = NULL;
  if (j) {
    cJSON *error = cJSON_GetObjectItemCaseSensitive(j, "error");
    message = cJSON_IsObject(error)
        ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"))
        : cJSON_GetStringValue(error);
    if (message) snprintf(out, outn, "%s", message);
  }
  cJSON_Delete(j);
}

static int valid_mime(const char *mime) {
  return mime && (!strcmp(mime, "image/jpeg") || !strcmp(mime, "image/png") ||
                  !strcmp(mime, "image/gif") || !strcmp(mime, "image/webp"));
}

static char *vision_call(const char *args_json, void *ud) {
  MotirisAgent *agent = ud;
  if (!agent) return err_json("vision tool has no agent context");
  const char *backend = motiris_transport_name(agent);
  if (backend && strcmp(backend, "auto") && strcmp(backend, "libcurl"))
    return err_json("vision_analyze requires the libcurl transport");

  cJSON *args = cJSON_Parse(args_json);
  if (!args) return err_json("cannot parse arguments");
  const char *path = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "path"));
  const char *mime = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "mime"));
  const char *question = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(args, "question"));
  if (!question) question = "Describe this image concisely.";
  if (!valid_mime(mime)) {
    cJSON_Delete(args);
    return err_json("unsupported mime; use image/jpeg, png, gif or webp");
  }
  if (!*question || strlen(question) > VISION_QUESTION_CAP) {
    cJSON_Delete(args);
    return err_json("question must contain 1..4096 bytes");
  }
  long max_bytes, timeout;
  if (env_long("MOTIRIS_VISION_MAX_BYTES", VISION_DEFAULT_MAX_BYTES,
               VISION_HARD_MAX_BYTES, &max_bytes)) {
    cJSON_Delete(args);
    return err_json("MOTIRIS_VISION_MAX_BYTES must be 1..1048576");
  }
  if (env_long("MOTIRIS_VISION_TIMEOUT_S", VISION_DEFAULT_TIMEOUT_S,
               VISION_MAX_TIMEOUT_S, &timeout)) {
    cJSON_Delete(args);
    return err_json("MOTIRIS_VISION_TIMEOUT_S must be 1..120");
  }

  unsigned char *raw = NULL;
  size_t raw_len = 0;
  char errb[256];
  if (read_image(path, (size_t)max_bytes, &raw, &raw_len,
                 errb, sizeof errb)) {
    cJSON_Delete(args);
    return err_json(errb);
  }
  char *data_url = image_data_url(raw, raw_len, mime);
  free(raw);
  if (!data_url) {
    cJSON_Delete(args);
    return err_json("image encoding allocation failed");
  }

  const char *model = getenv("MOTIRIS_VISION_MODEL");
  if (!model || !*model) model = motiris_model(agent);
  const char *url = getenv("MOTIRIS_VISION_BASE_URL");
  if (!url || !*url) url = motiris_base_url(agent);
  const char *key = getenv("MOTIRIS_VISION_API_KEY");
  if (!key || !*key) key = motiris_api_key(agent);
  if (!key || !*key) key = getenv("MOTIRIS_API_KEY");
  if (!model || !*model || strlen(model) > 255 ||
      !url || !*url || strlen(url) > 2047) {
    free(data_url);
    cJSON_Delete(args);
    return err_json("vision model or endpoint is missing/too long");
  }

  char *body = build_vision_request(model, question, data_url);
  free(data_url);
  cJSON_Delete(args);
  if (!body) return err_json("vision request assembly failed");

  long status = 0;
  char *response = post_vision(url, key, body, timeout, &status,
                               errb, sizeof errb);
  free(body);
  if (!response) return err_json(errb);
  if (status != 200) {
    char detail[240] = "no provider detail";
    provider_error_copy(response, detail, sizeof detail);
    free(response);
    return errf_json("vision provider HTTP %ld: %.200s", status, detail);
  }

  cJSON *j = cJSON_Parse(response);
  free(response);
  if (!j) return err_json("cannot parse vision provider response");
  cJSON *api_error = cJSON_GetObjectItemCaseSensitive(j, "error");
  if (api_error) {
    cJSON_Delete(j);
    return err_json("vision provider returned an error");
  }
  cJSON *choice = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
  cJSON *message = choice
      ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
  const char *description = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(message, "content"));
  if (!description || !*description) {
    cJSON_Delete(j);
    return err_json("vision provider returned no text description");
  }
  size_t dlen = strlen(description);
  size_t kept = dlen > VISION_DESCRIPTION_CAP ? VISION_DESCRIPTION_CAP : dlen;
  char *copy = malloc(kept + 1);
  if (!copy) {
    cJSON_Delete(j);
    return err_json("vision description allocation failed");
  }
  memcpy(copy, description, kept);
  copy[kept] = '\0';

  cJSON *result = cJSON_CreateObject();
  if (!result) {
    free(copy);
    cJSON_Delete(j);
    return err_json("vision result allocation failed");
  }
  cJSON_AddStringToObject(result, "description", copy);
  cJSON_AddBoolToObject(result, "truncated", dlen > kept);
  cJSON_AddNumberToObject(result, "input_bytes", (double)raw_len);
  char *printed = cJSON_PrintUnformatted(result);
  cJSON_Delete(result);
  cJSON_Delete(j);
  free(copy);
  return printed;
}

void motiris_register_vision_tools(MotirisAgent *a) {
  MotirisTool tool = {
    "vision_analyze",
    "Describe one local image with the configured vision provider. The image "
    "must be inside the workspace and is sent off-device; do not use for "
    "sensitive images unless the provider is trusted.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
    "\"description\":\"workspace-local image path\"},\"mime\":{"
    "\"type\":\"string\",\"enum\":[\"image/jpeg\",\"image/png\","
    "\"image/gif\",\"image/webp\"]},\"question\":{\"type\":\"string\","
    "\"maxLength\":4096}},\"required\":[\"path\",\"mime\"]}",
    vision_call, a
  };
  motiris_register_tool(a, &tool);
}
