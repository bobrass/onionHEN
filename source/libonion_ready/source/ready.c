/* Copyright (C) 2025 OnionHEN / LightningMods */

#include <onion/fs.h>
#include <onion/ready.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_name(const char *name) {
  if (!name || !*name) {
    return false;
  }
  for (const char *p = name; *p; ++p) {
    if (*p == '/' || *p == '.' || *p == '\\') {
      return false;
    }
  }
  return true;
}

bool onion_ready_path(const char *name, char *buf, size_t buflen) {
  if (!valid_name(name) || !buf || buflen < 32) {
    return false;
  }
  int n = snprintf(buf, buflen, "%s/%s", ONION_READY_ROOT, name);
  return n > 0 && (size_t)n < buflen;
}

static void ensure_root(void) {
  mkdir(ONION_SYSTEM_TMP_ROOT, 0777);
  mkdir(ONION_READY_ROOT, 0777);
}

static bool write_path(const char *path, const char *value) {
  if (!path || !value) {
    return false;
  }
  return write_file_atomic_with_mode(path, value, strlen(value), 0666);
}

static bool touch_path(const char *path) {
  return write_path(path, "1");
}

static bool path_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

bool onion_ready_signal(const char *name) {
  char path[256];
  if (!onion_ready_path(name, path, sizeof(path))) {
    return false;
  }
  ensure_root();
  bool ok = touch_path(path);
  return ok;
}

bool onion_ready_signal_pid(const char *name, pid_t pid) {
  char path[256];
  char value[32];
  if (pid <= 0 || !onion_ready_path(name, path, sizeof(path))) {
    return false;
  }
  int n = snprintf(value, sizeof(value), "%ld", (long)pid);
  if (n <= 0 || (size_t)n >= sizeof(value)) {
    return false;
  }

  ensure_root();
  return write_path(path, value);
}

bool onion_ready_clear(const char *name) {
  char path[256];
  if (!onion_ready_path(name, path, sizeof(path))) {
    return false;
  }
  unlink(path);
  return true;
}

bool onion_ready_is_set(const char *name) {
  char path[256];
  if (!onion_ready_path(name, path, sizeof(path))) {
    return false;
  }
  return path_exists(path);
}

static bool read_pid_path(const char *path, pid_t *pid_out) {
  char value[32];
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  size_t length = 0;
  bool read_error = false;
  while (length < sizeof(value)) {
    const ssize_t count = read(fd, value + length, sizeof(value) - length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      read_error = true;
      break;
    }
    if (count == 0)
      break;
    length += (size_t)count;
  }
  close(fd);
  if (read_error || length == 0 || length == sizeof(value)) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9')
      return false;
  }
  value[length] = '\0';

  errno = 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (errno != 0 || end != value + length || parsed <= 0 ||
      parsed > INT_MAX) {
    return false;
  }

  *pid_out = (pid_t)parsed;
  return (long)*pid_out == parsed;
}

bool onion_ready_read_pid(const char *name, pid_t *pid_out) {
  char path[256];
  if (!pid_out || !onion_ready_path(name, path, sizeof(path))) {
    return false;
  }
  return read_pid_path(path, pid_out);
}

bool onion_ready_matches_pid(const char *name, pid_t pid) {
  pid_t published = -1;
  return pid > 0 && onion_ready_read_pid(name, &published) && published == pid;
}

bool onion_ready_wait(const char *name, int timeout_ms, int poll_ms) {
  if (!valid_name(name)) {
    return false;
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }
  if (poll_ms < 50) {
    poll_ms = 50;
  }

  int waited = 0;
  while (waited <= timeout_ms) {
    if (onion_ready_is_set(name)) {
      return true;
    }
    if (waited >= timeout_ms) {
      break;
    }
    usleep((useconds_t)poll_ms * 1000);
    waited += poll_ms;
  }
  return onion_ready_is_set(name);
}

bool onion_ready_wait_pid(const char *name, pid_t pid, int timeout_ms,
                          int poll_ms) {
  if (!valid_name(name) || pid <= 0) {
    return false;
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }
  if (poll_ms < 50) {
    poll_ms = 50;
  }

  int waited = 0;
  while (waited <= timeout_ms) {
    if (onion_ready_matches_pid(name, pid)) {
      return true;
    }
    if (waited >= timeout_ms) {
      break;
    }
    usleep((useconds_t)poll_ms * 1000);
    waited += poll_ms;
  }
  return onion_ready_matches_pid(name, pid);
}
