/* tools.c - built-in tools for motiris.
 *
 * Tool contract: a tool is (name, description, parameters-json, handler).
 * The handler receives the arguments as JSON text and must return a
 * malloc'd JSON string (any shape); the agent wraps it as the model's
 * "tool" role message.
 */
#include "motiris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef MOTIRIS_NO_SHELL
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* -------- optional host shell: bounded subprocess and output -------- */
#define SHELL_DEFAULT_TIMEOUT_S 30L
#define SHELL_MAX_TIMEOUT_S 120L
#define SHELL_DEFAULT_STREAM_CAP 16384L
#define SHELL_DEFAULT_TOTAL_CAP 32768L
#define SHELL_HARD_OUTPUT_CAP 262144L
#define SHELL_POLL_MS 50

struct shell_limits {
  long timeout_ms;
  size_t stdout_cap, stderr_cap, total_cap;
};

struct shell_capture {
  char *data;
  size_t len;
  int truncated;
};

static char *shell_json_error(const char *message) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "stdout", "");
  cJSON_AddStringToObject(r, "stderr", "");
  cJSON_AddNumberToObject(r, "exit_code", -1);
  cJSON_AddNumberToObject(r, "signal", 0);
  cJSON_AddBoolToObject(r, "timed_out", 0);
  cJSON_AddBoolToObject(r, "truncated", 0);
  cJSON_AddBoolToObject(r, "stdout_truncated", 0);
  cJSON_AddBoolToObject(r, "stderr_truncated", 0);
  cJSON_AddStringToObject(r, "error", message);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static int shell_setting(const char *name, long fallback, long maximum,
                         long *value, char *error, size_t error_cap) {
  const char *raw = getenv(name);
  if (!raw || !*raw) {
    *value = fallback;
    return 0;
  }
  char *end = NULL;
  errno = 0;
  long parsed = strtol(raw, &end, 10);
  if (errno || !end || *end || parsed < 1 || parsed > maximum) {
    snprintf(error, error_cap, "%s must be 1..%ld", name, maximum);
    return -1;
  }
  *value = parsed;
  return 0;
}

static int shell_get_limits(struct shell_limits *limits,
                            char *error, size_t error_cap) {
  long timeout, out, err, total;
  if (shell_setting("MOTIRIS_SHELL_TIMEOUT_S", SHELL_DEFAULT_TIMEOUT_S,
                    SHELL_MAX_TIMEOUT_S, &timeout, error, error_cap) ||
      shell_setting("MOTIRIS_SHELL_STDOUT_MAX", SHELL_DEFAULT_STREAM_CAP,
                    SHELL_HARD_OUTPUT_CAP, &out, error, error_cap) ||
      shell_setting("MOTIRIS_SHELL_STDERR_MAX", SHELL_DEFAULT_STREAM_CAP,
                    SHELL_HARD_OUTPUT_CAP, &err, error, error_cap) ||
      shell_setting("MOTIRIS_SHELL_OUTPUT_MAX", SHELL_DEFAULT_TOTAL_CAP,
                    SHELL_HARD_OUTPUT_CAP, &total, error, error_cap))
    return -1;
  limits->timeout_ms = timeout * 1000L;
  limits->stdout_cap = (size_t)out;
  limits->stderr_cap = (size_t)err;
  limits->total_cap = (size_t)total;
  return 0;
}

static long shell_elapsed_ms(const struct timespec *start) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (now.tv_sec - start->tv_sec) * 1000L
       + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void shell_close(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void shell_kill_and_reap(pid_t pid, int *reaped, int *status) {
  /* setsid() in the child makes the command and its descendants one group. */
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  if (!*reaped) {
    while (waitpid(pid, status, 0) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    *reaped = 1;
  }
}

/* Append at most both per-stream and shared-total remaining bytes. The read
 * caller drains any excess instead of retaining it, so a full pipe cannot
 * deadlock against the other stream. realloc growth is exactly bounded. */
static int shell_append(struct shell_capture *capture, const char *bytes,
                        size_t count, size_t stream_cap, size_t *total_left) {
  size_t room = capture->len < stream_cap ? stream_cap - capture->len : 0;
  size_t keep = count < room ? count : room;
  if (keep > *total_left) keep = *total_left;
  if (keep) {
    char *grown = realloc(capture->data, capture->len + keep + 1);
    if (!grown) return -1;
    capture->data = grown;
    memcpy(capture->data + capture->len, bytes, keep);
    capture->len += keep;
    capture->data[capture->len] = '\0';
    *total_left -= keep;
  }
  if (keep != count) capture->truncated = 1;
  return 0;
}

static char *shell_result(struct shell_capture *out,
                          struct shell_capture *err, int status,
                          int timed_out, const char *error) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "stdout", out->data ? out->data : "");
  cJSON_AddStringToObject(r, "stderr", err->data ? err->data : "");
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  int signal_number = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  if (timed_out) {
    exit_code = -1;
    signal_number = SIGKILL;
  }
  cJSON_AddNumberToObject(r, "exit_code", exit_code);
  cJSON_AddNumberToObject(r, "signal", signal_number);
  cJSON_AddBoolToObject(r, "timed_out", timed_out);
  cJSON_AddBoolToObject(r, "truncated", out->truncated || err->truncated);
  cJSON_AddBoolToObject(r, "stdout_truncated", out->truncated);
  cJSON_AddBoolToObject(r, "stderr_truncated", err->truncated);
  if (error) cJSON_AddStringToObject(r, "error", error);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

static char *shell_call(const char *args_json, void *ud) {
  (void)ud;
  cJSON *args = cJSON_Parse(args_json);
  if (!args) return cJSON_PrintUnformatted(
      cJSON_CreateString("error: cannot parse arguments json"));
  cJSON *cmd = cJSON_GetObjectItemCaseSensitive(args, "command");
  if (!cJSON_IsString(cmd) || !cmd->valuestring) {
    cJSON_Delete(args);
    return cJSON_PrintUnformatted(cJSON_CreateString(
        "error: missing string field \"command\""));
  }
  const char *command = cmd->valuestring;

  char *ban = motiris_shell_policy_check(command);
  if (ban) {
    cJSON_Delete(args);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", ban);
    free(ban);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
  }

  struct shell_limits limits;
  char error[160] = {0};
  if (shell_get_limits(&limits, error, sizeof error)) {
    cJSON_Delete(args);
    return shell_json_error(error);
  }
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    cJSON_Delete(args);
    return shell_json_error("shell monotonic clock unavailable");
  }

  int outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1};
  if (pipe(outpipe)) {
    cJSON_Delete(args);
    return shell_json_error("shell stdout pipe failed");
  }
  if (pipe(errpipe)) {
    close(outpipe[0]); close(outpipe[1]);
    cJSON_Delete(args);
    return shell_json_error("shell stderr pipe failed");
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(outpipe[0]); close(outpipe[1]);
    close(errpipe[0]); close(errpipe[1]);
    cJSON_Delete(args);
    return shell_json_error("shell fork failed");
  }

  if (pid == 0) {
    close(outpipe[0]); close(errpipe[0]);
    if (setsid() < 0 || dup2(outpipe[1], STDOUT_FILENO) < 0
        || dup2(errpipe[1], STDERR_FILENO) < 0)
      _exit(126);
    close(outpipe[1]); close(errpipe[1]);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }
  cJSON_Delete(args);
  close(outpipe[1]);
  close(errpipe[1]);
  int outfd = outpipe[0], errfd = errpipe[0];
  int status = 0, reaped = 0, timed_out = 0;
  struct shell_capture out = {0}, err = {0};
  size_t total_left = limits.total_cap;

  int flags = fcntl(outfd, F_GETFL, 0);
  if (flags < 0 || fcntl(outfd, F_SETFL, flags | O_NONBLOCK) < 0)
    snprintf(error, sizeof error, "cannot configure shell stdout pipe");
  flags = fcntl(errfd, F_GETFL, 0);
  if (!error[0] && (flags < 0
      || fcntl(errfd, F_SETFL, flags | O_NONBLOCK) < 0))
    snprintf(error, sizeof error, "cannot configure shell stderr pipe");

  while (!error[0] && (!reaped || outfd >= 0 || errfd >= 0)) {
    long elapsed = shell_elapsed_ms(&start);
    if (elapsed < 0) {
      snprintf(error, sizeof error, "shell monotonic clock failed");
      break;
    }
    long remaining = limits.timeout_ms - elapsed;
    if (remaining <= 0) {
      timed_out = 1;
      snprintf(error, sizeof error, "shell command timed out after %ld s",
               limits.timeout_ms / 1000L);
      break;
    }

    if (!reaped) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        reaped = 1;
      } else if (waited < 0 && errno != EINTR) {
        snprintf(error, sizeof error, "shell waitpid failed");
        break;
      }
    }

    struct pollfd fds[2];
    fds[0].fd = outfd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = errfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    nfds_t nfds = (outfd >= 0 || errfd >= 0) ? 2 : 0;
    int wait_ms = remaining < SHELL_POLL_MS ? (int)remaining : SHELL_POLL_MS;
    int polled = poll(fds, nfds, wait_ms);
    if (polled < 0 && errno != EINTR) {
      snprintf(error, sizeof error, "shell output poll failed");
      break;
    }
    if (polled <= 0) continue;

    for (int i = 0; i < 2; i++) {
      int *fd = i == 0 ? &outfd : &errfd;
      struct shell_capture *capture = i == 0 ? &out : &err;
      size_t stream_cap = i == 0 ? limits.stdout_cap : limits.stderr_cap;
      if (*fd < 0 || !(fds[i].revents
          & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
      if (fds[i].revents & POLLNVAL) {
        snprintf(error, sizeof error, "shell output descriptor failed");
        break;
      }
      char chunk[4096];
      ssize_t count = read(*fd, chunk, sizeof chunk);
      if (count > 0) {
        if (shell_append(capture, chunk, (size_t)count, stream_cap,
                         &total_left)) {
          snprintf(error, sizeof error, "shell output allocation failed");
          break;
        }
      } else if (count == 0) {
        shell_close(fd);
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        snprintf(error, sizeof error, "shell output read failed");
        break;
      }
    }
  }

  if (error[0]) {
    shell_kill_and_reap(pid, &reaped, &status);
    shell_close(&outfd);
    shell_close(&errfd);
  } else {
    shell_close(&outfd);
    shell_close(&errfd);
  }
  char *result = shell_result(&out, &err, status, timed_out,
                              error[0] ? error : NULL);
  free(out.data);
  free(err.data);
  return result;
}
#endif /* MOTIRIS_NO_SHELL */

/* ---------------- time: current UTC clock ---------------- */
static char *time_call(const char *args_json, void *ud) {
  (void)args_json; (void)ud;
  char iso[64];
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "utc_iso8601", iso);
  cJSON_AddNumberToObject(r, "unix_seconds", (double)now);
  char *s = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return s;
}

/* ---------------- registration ---------------- */
static const MotirisTool core_tools[] = {
#ifndef MOTIRIS_NO_SHELL
  { "shell",
    "Run a shell command via /bin/sh with bounded stdout, stderr and "
    "execution time. The command runs with the agent's uid - it can modify "
    "the system. Use for file inspection, git, build tools, networking.",
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
    "\"description\":\"shell command line\"}},\"required\":[\"command\"]}",
    shell_call, NULL },
#endif
  { "time",
    "Get the current UTC time as ISO-8601 and unix seconds.",
    "{\"type\":\"object\",\"properties\":{}}",
    time_call, NULL },
};

int motiris_register_core_tools_count(void) {
  return (int)(sizeof core_tools / sizeof core_tools[0]);
}
const MotirisTool *motiris_core_tools(void) { return core_tools; }
