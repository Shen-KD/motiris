/* tts.c - optional host-only external text-to-speech adapter.
 *
 * MOTIRIS_TTS_CMD names one trusted executable. The final reply is written
 * to its stdin (no shell); the engine owns playback/audio storage. Stdout and
 * stderr go to /dev/null so an engine cannot corrupt the textual REPL stream.
 * A nonblocking pipe, monotonic deadline and process-group kill bound every
 * invocation. No text or audio is retained between calls.
 */
#include "tts.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TTS_DEFAULT_TIMEOUT_S 10L
#define TTS_MAX_TIMEOUT_S 120L
#define TTS_POLL_MS 50

static void diagnostic(char *dst, size_t cap, const char *msg) {
  if (dst && cap) snprintf(dst, cap, "%s", msg);
}

static long milliseconds_since(const struct timespec *start) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (now.tv_sec - start->tv_sec) * 1000L
       + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void kill_and_reap(pid_t pid) {
  /* The child calls setsid(), so this also stops engine-descendant players. */
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static int parse_timeout(char *diag, size_t diag_cap) {
  const char *raw = getenv("MOTIRIS_TTS_TIMEOUT_S");
  if (!raw || !*raw) return (int)TTS_DEFAULT_TIMEOUT_S;
  char *end = NULL;
  errno = 0;
  long value = strtol(raw, &end, 10);
  if (errno || !end || *end || value < 1 || value > TTS_MAX_TIMEOUT_S) {
    diagnostic(diag, diag_cap, "MOTIRIS_TTS_TIMEOUT_S must be 1..120");
    return -1;
  }
  return (int)value;
}

int motiris_tts_enabled(void) {
  const char *cmd = getenv("MOTIRIS_TTS_CMD");
  return cmd && *cmd;
}

int motiris_tts_speak(const char *text, char *diag, size_t diag_cap) {
  if (diag && diag_cap) diag[0] = '\0';
  if (!text) {
    diagnostic(diag, diag_cap, "empty reply");
    return -1;
  }

  size_t len = 0;
  while (len < MOTIRIS_TTS_TEXT_CAP && text[len]) len++;
  int truncated = text[len] != '\0';
  if (!len) {
    diagnostic(diag, diag_cap, "empty reply");
    return -1;
  }

  int timeout_s = parse_timeout(diag, diag_cap);
  if (timeout_s < 0) return -1;
  const char *cmd = getenv("MOTIRIS_TTS_CMD");
  if (!cmd || !*cmd) {
    diagnostic(diag, diag_cap, "MOTIRIS_TTS_CMD is not configured");
    return -1;
  }

  int input[2];
  if (pipe(input) != 0) {
    diagnostic(diag, diag_cap, "cannot create engine input pipe");
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(input[0]);
    close(input[1]);
    diagnostic(diag, diag_cap, "cannot start engine");
    return -1;
  }
  if (pid == 0) {
    close(input[1]);
    if (setsid() < 0) _exit(126);
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd < 0 || dup2(input[0], STDIN_FILENO) < 0
        || dup2(nullfd, STDOUT_FILENO) < 0
        || dup2(nullfd, STDERR_FILENO) < 0)
      _exit(126);
    close(input[0]);
    if (nullfd > STDERR_FILENO) close(nullfd);
    execl(cmd, cmd, (char *)NULL);
    _exit(127);
  }

  close(input[0]);
  int fd = input[1];
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    kill_and_reap(pid);
    diagnostic(diag, diag_cap, "cannot configure engine input pipe");
    return -1;
  }

  /* A backend may close stdin early. Do not let SIGPIPE terminate the REPL. */
  struct sigaction ignore = {0}, old = {0};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (sigaction(SIGPIPE, &ignore, &old) != 0) {
    close(fd);
    kill_and_reap(pid);
    diagnostic(diag, diag_cap, "cannot configure pipe signal handling");
    return -1;
  }

  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    close(fd);
    sigaction(SIGPIPE, &old, NULL);
    kill_and_reap(pid);
    diagnostic(diag, diag_cap, "monotonic clock unavailable");
    return -1;
  }

  size_t offset = 0;
  int status = 0;
  int result = -1;
  int failed = 0;
  for (;;) {
    long elapsed = milliseconds_since(&start);
    if (elapsed < 0) {
      diagnostic(diag, diag_cap, "monotonic clock failed");
      kill_and_reap(pid);
      failed = 1;
      break;
    }
    long remaining = timeout_s * 1000L - elapsed;
    if (remaining <= 0) {
      if (diag && diag_cap)
        snprintf(diag, diag_cap, "engine timed out after %d s", timeout_s);
      kill_and_reap(pid);
      failed = 1;
      break;
    }

    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      if (offset != len) {
        diagnostic(diag, diag_cap, "engine exited before accepting all text");
      } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result = truncated ? 1 : 0;
        if (truncated && diag && diag_cap)
          snprintf(diag, diag_cap, "spoke only the first %u bytes; full text preserved",
                   (unsigned)MOTIRIS_TTS_TEXT_CAP);
      } else if (WIFEXITED(status)) {
        if (diag && diag_cap)
          snprintf(diag, diag_cap, "engine exited with status %d",
                   WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        if (diag && diag_cap)
          snprintf(diag, diag_cap, "engine terminated by signal %d",
                   WTERMSIG(status));
      } else {
        diagnostic(diag, diag_cap, "engine did not complete");
      }
      break;
    }
    if (waited < 0 && errno != EINTR) {
      diagnostic(diag, diag_cap, "cannot wait for engine");
      kill_and_reap(pid);
      failed = 1;
      break;
    }

    if (fd >= 0) {
      struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
      int wait_ms = remaining < TTS_POLL_MS ? (int)remaining : TTS_POLL_MS;
      int polled = poll(&pfd, 1, wait_ms);
      if (polled < 0 && errno != EINTR) {
        diagnostic(diag, diag_cap, "engine input poll failed");
        kill_and_reap(pid);
        failed = 1;
        break;
      }
      if (polled > 0 && (pfd.revents & POLLOUT)) {
        ssize_t written = write(fd, text + offset, len - offset);
        if (written > 0) {
          offset += (size_t)written;
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK
                   && errno != EINTR) {
          diagnostic(diag, diag_cap, "engine closed its text input");
          kill_and_reap(pid);
          failed = 1;
          break;
        }
      } else if (polled > 0
                 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        diagnostic(diag, diag_cap, "engine closed its text input");
        kill_and_reap(pid);
        failed = 1;
        break;
      }
      if (offset == len) {
        close(fd);
        fd = -1; /* EOF tells the engine that the bounded message is complete. */
      }
    } else {
      int wait_ms = remaining < TTS_POLL_MS ? (int)remaining : TTS_POLL_MS;
      int polled = poll(NULL, 0, wait_ms);
      (void)polled; /* EINTR simply rechecks the child and monotonic deadline. */
    }
  }

  if (fd >= 0) close(fd);
  sigaction(SIGPIPE, &old, NULL);
  if (failed) result = -1;
  return result;
}
