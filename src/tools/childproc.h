/* Private bounded host-child runner shared by browser and subagent tools. */
#ifndef MOTIRIS_CHILDPROC_H
#define MOTIRIS_CHILDPROC_H

#include <stddef.h>

#define CHILDPROC_ERROR_CAP 192

typedef struct ChildProcResult {
  char *output;             /* combined stdout/stderr, always NUL-terminated */
  size_t output_len;
  int truncated;
  int timed_out;
  int exit_code;
  int signal_number;
  char error[CHILDPROC_ERROR_CAP];
} ChildProcResult;

/* Run argv with one monotonic timeout. Output is drained and discarded after
 * output_cap bytes. The child and its descendants share a new process group;
 * timeout/setup/I/O failure uses TERM, a bounded grace, KILL and direct-child
 * reap. Normal return also kills same-group background members left by an
 * exited leader. The caller frees result.output with childproc_free(). */
void childproc_run(const char *name, char *const argv[],
                   const char *timeout_env, long default_timeout_s,
                   size_t output_cap, ChildProcResult *result);
void childproc_free(ChildProcResult *result);

#endif
