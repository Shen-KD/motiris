/* cron.c - scheduled agent runs (issue #11).
 *
 *   motiris --cron [jobs.json] [--once]
 *
 * jobs.json:
 *   [ {"id":"daily","interval_s":3600,"prompt":"summarize today"},
 *     {"interval_s":86400,"prompt":"...","model":"optional-override"} ]
 *
 * Each job runs on its own schedule (interval since last run); output
 * appends to ~/.local/share/motiris/cron/<id>.log with timestamps.
 * --once: run every due job once and exit (used by tests); without it,
 * the process stays resident and re-checks every second.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS 16

typedef struct {
  char id[64];
  char prompt[1024];
  long interval_s;
  time_t last_run;
} Job;

static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static char *log_dir(void) {
  const char *h = getenv("HOME");
  if (!h) h = ".";
  size_t n = strlen(h) + 40;
  char *d = malloc(n);
  snprintf(d, n, "%s/.local/share/motiris/cron", h);
  mkdir_p(d);
  return d;
}

static int load_jobs(const char *file, Job *jobs, int *n) {
  FILE *f = fopen(file, "r");
  if (!f) { fprintf(stderr, "motiris cron: cannot open %s\n", file); return -1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 1 << 20) { fclose(f); return -1; }
  char *buf = malloc((size_t)sz + 1);
  buf[fread(buf, 1, (size_t)sz, f)] = '\0';
  fclose(f);
  cJSON *j = cJSON_Parse(buf);
  free(buf);
  if (!j || !cJSON_IsArray(j)) {
    if (j) cJSON_Delete(j);
    fprintf(stderr, "motiris cron: %s is not a job array\n", file);
    return -1;
  }
  *n = 0;
  cJSON *item;
  cJSON_ArrayForEach(item, j) {
    if (*n >= MAX_JOBS) break;
    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "id"));
    const char *prompt = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(item, "prompt"));
    double iv = cJSON_GetObjectItemCaseSensitive(item, "interval_s")
                    ? cJSON_GetNumberValue(
                          cJSON_GetObjectItemCaseSensitive(item, "interval_s"))
                    : 0;
    if (!prompt || !*prompt || iv <= 0) continue;
    Job *jb = &jobs[*n];
    snprintf(jb->id, sizeof jb->id, "%s", id ? id : "job");
    snprintf(jb->prompt, sizeof jb->prompt, "%s", prompt);
    jb->interval_s = (long)iv;
    jb->last_run = 0;
    (*n)++;
  }
  cJSON_Delete(j);
  return 0;
}

static void run_job(Job *jb, const char *filename, int verbose) {
  MotirisAgent *a = motiris_new();
  motiris_apply_config(a);
  motiris_register_core_tools(a);
  if (motiris_tools_enabled(a)) motiris_register_file_tools(a);
  motiris_add_user(a, jb->prompt);
  int rc = motiris_run(a);

  char *dir = log_dir();
  size_t pn = strlen(dir) + strlen(jb->id) + 24;
  char *path = malloc(pn);
  snprintf(path, pn, "%s/%s.log", dir, jb->id);
  FILE *f = fopen(path, "a");
  if (f) {
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "[%s] rc=%d %s\n", ts, rc, jb->prompt);
    fclose(f);
  }
  if (verbose)
    fprintf(stderr, "[motiris cron] %s: rc=%d (log %s)\n", jb->id, rc, path);
  free(path);
  free(dir);
  motiris_free(a);
  jb->last_run = time(NULL);
  (void)filename;
}

int motiris_cron_run(const char *file, int once, int verbose) {
  Job jobs[MAX_JOBS];
  int n = 0;
  if (load_jobs(file, jobs, &n)) return 1;
  if (n == 0) { fprintf(stderr, "motiris cron: no jobs\n"); return 1; }

  for (;;) {
    time_t now = time(NULL);
    int ran = 0;
    for (int i = 0; i < n; i++) {
      if (now - jobs[i].last_run >= jobs[i].interval_s) {
        run_job(&jobs[i], file, verbose);
        ran = 1;
      }
    }
    if (once) return 0;
    if (ran) {
      /* after a burst, recompute against fresh timestamps */
      continue;
    }
    sleep(1);
  }
}