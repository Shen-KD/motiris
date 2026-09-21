/* sessions.c - shared session-log helpers: state dir, listing, append.
 *
 * Motiris keeps append-only session logs as <tag><line> rows (U=user,
 * A=assistant, T=tool) under $HOME/.local/share/motiris/<sub>/<id>.log
 * (sub = "gateway", "sessions" for the repl, ...).  Everything that
 * reads or writes these logs goes through this module so the CLI
 * (--sessions), the repl (/sessions) and the gateway stay in sync.
 */
#include "motiris.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

/* state dir for <sub>; mkdir -p then return strdup'd path (NULL if no HOME) */
char *motiris_state_dir(const char *sub) {
  const char *h = getenv("HOME");
  if (!h) return NULL;
  size_t n = strlen(h) + strlen(sub) + 40;
  char *d = malloc(n);
  snprintf(d, n, "%s/.local/share/motiris/%s", h, sub);
  mkdir_p(d);
  return d;
}

/* list *.log under <sub>; with term, grep it (same as --sessions) */
int motiris_session_list(const char *sub, const char *term) {
  char *dir = motiris_state_dir(sub);
  if (!dir) { printf("no sessions yet\n"); return 0; }
  DIR *d = opendir(dir);
  if (!d) { free(dir); printf("no sessions yet\n"); return 0; }
  struct dirent *e;
  int shown = 0;
  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    if (len < 5 || strcmp(e->d_name + len - 4, ".log")) continue;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (term) {
      FILE *f = fopen(path, "r");
      if (f) {
        char line[65536];
        long ln = 0;
        while (fgets(line, sizeof line, f)) {
          ln++;
          if (strstr(line, term))
            printf("%s:%ld:%s", e->d_name, ln, line);
        }
        fclose(f);
      }
      shown = 1;
    } else {
      FILE *f = fopen(path, "r");
      long lines = 0;
      if (f) {
        int c;
        while ((c = fgetc(f)) != EOF) if (c == '\n') lines++;
        fclose(f);
      }
      printf("%-40s %ld lines\n", e->d_name, lines);
      shown = 1;
    }
  }
  closedir(d);
  free(dir);
  if (!shown && !term) printf("no sessions yet\n");
  return 0;
}

/* append one <tag>row to <sub>/<id>.log; returns 0 on success */
int motiris_session_append(const char *sub, const char *id, char tag,
                           const char *line) {
  char *dir = motiris_state_dir(sub);
  if (!dir) return 1;
  size_t n = strlen(dir) + strlen(id) + 8;
  char *path = malloc(n);
  snprintf(path, n, "%s/%s.log", dir, id);
  free(dir);
  FILE *f = fopen(path, "a");
  if (!f) { free(path); return 1; }
  fprintf(f, "%c%s\n", tag, line ? line : "");
  fclose(f);
  free(path);
  return 0;
}