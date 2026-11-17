/* childproc.c - bounded host-only child execution for browser/subagent. */
#include "childproc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILDPROC_MAX_TIMEOUT_S 120L
#define CHILDPROC_GRACE_MS 1000L
#define CHILDPROC_WAIT_SLICE_MS 50L

static void childproc_error(ChildProcResult *result, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(result->error, sizeof result->error, fmt, ap);
  va_end(ap);
}

static long childproc_remaining(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  long ms = (deadline->tv_sec - now.tv_sec) * 1000L
          + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
  return ms < 0 ? 0 : ms;
}

static void childproc_close(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

/* Signal the group before reaping its leader. If the leader is already gone,
 * descendants still receive KILL rather than escaping the cleanup budget. */
static int childproc_terminate(pid_t pid, int reaped, int *status) {
  kill(-pid, SIGTERM);
  if (!reaped) kill(pid, SIGTERM);

  if (reaped) {
    kill(-pid, SIGKILL);
    return 1;
  }

  struct timespec deadline;
  int have_deadline = clock_gettime(CLOCK_MONOTONIC, &deadline) == 0;
  if (have_deadline) {
    deadline.tv_sec += CHILDPROC_GRACE_MS / 1000L;
    deadline.tv_nsec += (CHILDPROC_GRACE_MS % 1000L) * 1000000L;
  }

  while (have_deadline) {
    pid_t waited;
    do {
      waited = waitpid(pid, status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == pid) {
      kill(-pid, SIGKILL);
      return 1;
    }
    if (waited < 0 && errno == ECHILD) {
      kill(-pid, SIGKILL);
      return 0;
    }
    long remaining = childproc_remaining(&deadline);
    if (remaining <= 0) break;
    long slice = remaining < CHILDPROC_WAIT_SLICE_MS
               ? remaining : CHILDPROC_WAIT_SLICE_MS;
    (void)poll(NULL, 0, (int)slice);
  }

  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  /* As with shell/MCP, a task in uninterruptible kernel sleep is outside the
   * userspace deadline guarantee; the direct child is reaped whenever possible. */
  for (;;) {
    pid_t waited = waitpid(pid, status, 0);
    if (waited == pid) return 1;
    if (waited < 0 && errno == EINTR) continue;
    return 0;
  }
}

static int childproc_append(ChildProcResult *result, const char *bytes,
                            size_t count, size_t output_cap) {
  size_t room = result->output_len < output_cap
              ? output_cap - result->output_len : 0;
  size_t keep = count < room ? count : room;
  if (keep) {
    char *grown = realloc(result->output, result->output_len + keep + 1);
    if (!grown) return -1;
    result->output = grown;
    memcpy(result->output + result->output_len, bytes, keep);
    result->output_len += keep;
    result->output[result->output_len] = '\0';
  }
  if (keep != count) result->truncated = 1;
  return 0;
}

static long childproc_timeout(const char *timeout_env, long fallback,
                              ChildProcResult *result) {
  const char *raw = getenv(timeout_env);
  if (!raw || !*raw) return fallback;
  char *end = NULL;
  errno = 0;
  long value = strtol(raw, &end, 10);
  if (errno || !end || *end || value < 1 || value > CHILDPROC_MAX_TIMEOUT_S) {
    childproc_error(result, "%s must be 1..%ld", timeout_env,
                    CHILDPROC_MAX_TIMEOUT_S);
    return -1;
  }
  return value;
}

void childproc_run(const char *name, char *const argv[],
                   const char *timeout_env, long default_timeout_s,
                   size_t output_cap, ChildProcResult *result) {
  memset(result, 0, sizeof *result);
  result->exit_code = -1;
  result->output = malloc(1);
  if (!result->output) {
    childproc_error(result, "%s output allocation failed", name);
    return;
  }
  result->output[0] = '\0';

  long timeout_s = childproc_timeout(timeout_env, default_timeout_s, result);
  if (timeout_s < 0) return;

  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
    childproc_error(result, "%s monotonic clock unavailable", name);
    return;
  }
  deadline.tv_sec += timeout_s;

  int outpipe[2] = {-1, -1};
  if (pipe(outpipe) != 0) {
    childproc_error(result, "%s pipe failed: %.100s", name, strerror(errno));
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    childproc_error(result, "%s fork failed: %.100s", name, strerror(errno));
    close(outpipe[0]);
    close(outpipe[1]);
    return;
  }
  if (pid == 0) {
    close(outpipe[0]);
    if (setsid() < 0 || dup2(outpipe[1], STDOUT_FILENO) < 0
        || dup2(outpipe[1], STDERR_FILENO) < 0)
      _exit(126);
    close(outpipe[1]);
    execvp(argv[0], argv);
    _exit(127);
  }

  close(outpipe[1]);
  int fd = outpipe[0];
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    childproc_error(result, "%s cannot configure output pipe: %.100s",
                    name, strerror(errno));

  int status = 0, reaped = 0;
  while (!result->error[0] && (!reaped || fd >= 0)) {
    long remaining = childproc_remaining(&deadline);
    if (remaining < 0) {
      childproc_error(result, "%s monotonic clock failed", name);
      break;
    }
    if (remaining == 0) {
      result->timed_out = 1;
      childproc_error(result, "%s timed out after %ld s", name, timeout_s);
      break;
    }

    if (!reaped) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        reaped = 1;
      } else if (waited < 0 && errno != EINTR) {
        childproc_error(result, "%s waitpid failed: %.100s",
                        name, strerror(errno));
        break;
      }
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    nfds_t nfds = fd >= 0 ? 1 : 0;
    long slice = remaining < CHILDPROC_WAIT_SLICE_MS
               ? remaining : CHILDPROC_WAIT_SLICE_MS;
    int ready = poll(&pfd, nfds, (int)slice);
    if (ready < 0) {
      if (errno == EINTR) continue;
      childproc_error(result, "%s output poll failed: %.100s",
                      name, strerror(errno));
      break;
    }
    if (ready == 0 || fd < 0) continue;
    if (pfd.revents & POLLNVAL) {
      childproc_error(result, "%s invalid output descriptor", name);
      break;
    }
    if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) continue;

    char chunk[4096];
    ssize_t count = read(fd, chunk, sizeof chunk);
    if (count > 0) {
      if (childproc_append(result, chunk, (size_t)count, output_cap) != 0) {
        childproc_error(result, "%s output allocation failed", name);
        break;
      }
    } else if (count == 0) {
      childproc_close(&fd);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      childproc_error(result, "%s output read failed: %.100s",
                      name, strerror(errno));
      break;
    }
  }

  childproc_close(&fd);
  if (result->error[0]) {
    reaped = childproc_terminate(pid, reaped, &status);
  } else if (reaped) {
    /* A successful leader must not leave same-group background work after
     * the bounded tool call returns (it may have closed the output pipe). */
    kill(-pid, SIGKILL);
  }

  if (reaped) {
    result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result->signal_number = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  }
  if (!result->error[0] && WIFEXITED(status) && WEXITSTATUS(status) != 0)
    childproc_error(result, "%s exited with status %d", name,
                    WEXITSTATUS(status));
  else if (!result->error[0] && WIFSIGNALED(status))
    childproc_error(result, "%s terminated by signal %d", name,
                    WTERMSIG(status));
}

void childproc_free(ChildProcResult *result) {
  free(result->output);
  result->output = NULL;
  result->output_len = 0;
}
