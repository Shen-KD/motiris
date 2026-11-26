/* Test-only LD_PRELOAD interposer: drop REPL async self-pipe wakeups.
 * The motiris process uses one-byte zero writes on a nonblocking pipe for UI
 * and worker-completion notifications. PTY and file writes pass through. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static ssize_t (*real_write)(int, const void *, size_t);
static _Atomic unsigned long dropped_wakes;
static int drop_count_fd = -1;

__attribute__((constructor)) static void init_write(void) {
  real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
  const char *path = getenv("MOTIRIS_TEST_WAKE_DROP_FILE");
  if (path && *path)
    drop_count_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

__attribute__((destructor)) static void save_drop_count(void) {
  if (drop_count_fd < 0) return;
  char buffer[64];
  int len = snprintf(buffer, sizeof buffer, "%lu\n",
                     atomic_load_explicit(&dropped_wakes,
                                          memory_order_relaxed));
  if (len > 0 && (size_t)len < sizeof buffer)
    (void)real_write(drop_count_fd, buffer, (size_t)len);
  close(drop_count_fd);
}

ssize_t write(int fd, const void *buffer, size_t count) {
  if (count == 1 && buffer && *(const unsigned char *)buffer == 0) {
    struct stat status;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && (flags & O_NONBLOCK) && fstat(fd, &status) == 0 &&
        S_ISFIFO(status.st_mode)) {
      atomic_fetch_add_explicit(&dropped_wakes, 1, memory_order_relaxed);
      return 1;
    }
  }
  return real_write(fd, buffer, count);
}
