/* Copyright (C) 2025 OnionHEN / LightningMods */

#include <onion/fs.h>
#include <onion/log.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_directory_entry(const char *path, unsigned char type) {
  struct stat st;
  if (type == DT_DIR) {
    return true;
  }
  if (type != DT_UNKNOWN || lstat(path, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

static bool is_real_directory_path(const char *path) {
  char trimmed_path[1024];
  const char *checked_path = path;
  const size_t original_length = strlen(path);
  size_t length = original_length;

  /* lstat("link/" or "link/.") follows the final symlink. */
  for (;;) {
    while (length > 1 && path[length - 1] == '/')
      --length;
    if (length > 2 && path[length - 2] == '/' && path[length - 1] == '.') {
      length -= 2;
      continue;
    }
    break;
  }
  if (length != original_length) {
    if (length >= sizeof(trimmed_path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    memcpy(trimmed_path, path, length);
    trimmed_path[length] = '\0';
    checked_path = trimmed_path;
  }

  struct stat st;
  if (lstat(checked_path, &st) != 0)
    return false;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  return true;
}

static bool validate_rmtree_target(const char *path) {
  if (!path || !path[0]) {
    errno = EINVAL;
    return false;
  }

  bool has_component = false;
  const char *component = path;
  while (*component) {
    while (*component == '/')
      ++component;
    if (!*component)
      break;

    const char *end = component;
    while (*end && *end != '/')
      ++end;
    const size_t length = (size_t)(end - component);
    if ((length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.')) {
      errno = EINVAL;
      return false;
    }
    has_component = true;
    component = end;
  }

  if (!has_component) {
    errno = EBUSY;
    return false;
  }
  return true;
}

bool if_exists(const char *path) {
  struct stat buffer;
  if (!path) {
    return false;
  }
  return stat(path, &buffer) == 0;
}

bool touch_file(const char *path) {
  if (!path || path[0] == '\0') {
    errno = EINVAL;
    return false;
  }
  const int fd = open(path, O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW, 0777);
  if (fd < 0) {
    LOG_ERROR("touch_file: open %s failed (%s)", path, strerror(errno));
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    const int saved_errno = errno;
    close(fd);
    LOG_ERROR("touch_file: stat %s failed (%s)", path, strerror(saved_errno));
    errno = saved_errno;
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    LOG_ERROR("touch_file: refusing non-regular file %s", path);
    errno = EINVAL;
    return false;
  }
  if (ftruncate(fd, 0) != 0) {
    const int saved_errno = errno;
    close(fd);
    LOG_ERROR("touch_file: truncate %s failed (%s)", path,
              strerror(saved_errno));
    errno = saved_errno;
    return false;
  }
  if (close(fd) != 0) {
    const int saved_errno = errno;
    LOG_ERROR("touch_file: close %s failed (%s)", path,
              strerror(saved_errno));
    errno = saved_errno;
    return false;
  }
  return true;
}

bool write_file_atomic(const char *path, const void *data, size_t size) {
  char temp_path[1024];
  int fd = -1;

  if (!path || path[0] == '\0' || (size > 0 && !data))
    return false;

  /* Never truncate or remove another writer's temporary file. */
  for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
    const int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d.%u", path,
                           (int)getpid(), attempt);
    if (n < 0 || (size_t)n >= sizeof(temp_path))
      return false;

    fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, 0777);
    if (fd >= 0 || errno != EEXIST)
      break;
  }
  if (fd < 0) {
    LOG_ERROR("write_file_atomic: open %s failed (%s)", temp_path,
              strerror(errno));
    return false;
  }

  const uint8_t *cursor = (const uint8_t *)data;
  size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = write(fd, cursor, remaining);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      LOG_ERROR("write_file_atomic: write %s failed (%s)", path,
                written < 0 ? strerror(errno) : "zero-byte write");
      close(fd);
      unlink(temp_path);
      return false;
    }
    cursor += (size_t)written;
    remaining -= (size_t)written;
  }

  if (fsync(fd) != 0) {
    LOG_ERROR("write_file_atomic: fsync %s failed (%s)", path, strerror(errno));
    close(fd);
    unlink(temp_path);
    return false;
  }
  if (close(fd) != 0) {
    LOG_ERROR("write_file_atomic: close %s failed (%s)", path, strerror(errno));
    unlink(temp_path);
    return false;
  }
  if (rename(temp_path, path) != 0) {
    LOG_ERROR("write_file_atomic: rename %s failed (%s)", path, strerror(errno));
    unlink(temp_path);
    return false;
  }
  return true;
}

static bool mkdir_or_existing_directory(const char *path) {
  if (mkdir(path, 0777) == 0) {
    return true;
  }
  if (errno != EEXIST) {
    return false;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  return true;
}

bool mkdir_tree(const char *path) {
  char current[1024];
  size_t n;
  size_t i;

  if (path == NULL || path[0] == '\0') {
    return false;
  }
  n = strlen(path);
  if (n >= sizeof(current)) {
    return false;
  }
  for (i = 0; i < n; ++i) {
    current[i] = path[i];
    current[i + 1] = '\0';
    if (path[i] != '/' || i == 0) {
      continue;
    }
    current[i] = '\0';
    if (current[0] != '\0' && !mkdir_or_existing_directory(current)) {
      return false;
    }
    current[i] = '/';
  }
  return mkdir_or_existing_directory(current);
}

static bool make_child_path(char *child, size_t child_size, const char *path,
                            const char *name) {
  const int written = snprintf(child, child_size, "%s/%s", path, name);
  if (written < 0 || (size_t)written >= child_size) {
    errno = ENAMETOOLONG;
    LOG_ERROR("Path too long below directory %s", path);
    return false;
  }
  return true;
}

static bool count_tree_entries(const char *path, size_t *out_count) {
  if (!is_real_directory_path(path)) {
    LOG_ERROR("Path is not a directory: %s: %s", path, strerror(errno));
    return false;
  }
  DIR *dir = opendir(path);
  size_t count = 1; /* The directory itself. */
  if (dir == NULL) {
    LOG_ERROR("Error opening directory %s: %s", path, strerror(errno));
    return false;
  }

  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) {
        LOG_ERROR("Error reading directory %s: %s", path, strerror(errno));
        ok = false;
      }
      break;
    }
    char child[1024];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!make_child_path(child, sizeof(child), path, entry->d_name)) {
      ok = false;
      break;
    }

    size_t child_count = 1;
    if (is_directory_entry(child, entry->d_type) &&
        !count_tree_entries(child, &child_count)) {
      ok = false;
      break;
    }
    if (child_count > SIZE_MAX - count) {
      errno = EOVERFLOW;
      LOG_ERROR("Directory entry count overflow under %s", path);
      ok = false;
      break;
    }
    count += child_count;
  }
  if (closedir(dir) != 0) {
    LOG_ERROR("Error closing directory %s: %s", path, strerror(errno));
    ok = false;
  }
  if (ok) {
    *out_count = count;
  }
  return ok;
}

static bool rmtree_impl(const char *path, onion_fs_progress_fn progress,
                        void *user, size_t *completed, size_t total) {
  if (!path) {
    return false;
  }

  if (!is_real_directory_path(path)) {
    LOG_ERROR("Path is not a directory: %s: %s", path, strerror(errno));
    return false;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    LOG_ERROR("Error opening directory %s", path);
    return false;
  }

  bool ok = true;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) {
        LOG_ERROR("Error reading directory %s: %s", path, strerror(errno));
        ok = false;
      }
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char child[1024];
    if (!make_child_path(child, sizeof(child), path, entry->d_name)) {
      ok = false;
      continue;
    }

    if (is_directory_entry(child, entry->d_type)) {
      if (!rmtree_impl(child, progress, user, completed, total)) {
        ok = false;
      }
    } else if (unlink(child) != 0) {
      LOG_ERROR("Error deleting file %s: %s", child, strerror(errno));
      ok = false;
    } else {
      if (completed != NULL) {
        ++(*completed);
        if (progress != NULL) {
          progress(*completed, total, user);
        }
      }
    }
  }

  if (closedir(dir) != 0) {
    LOG_ERROR("Error closing directory %s: %s", path, strerror(errno));
    ok = false;
  }

  if (rmdir(path) != 0) {
    LOG_ERROR("Error deleting folder %s: %s", path, strerror(errno));
    ok = false;
  } else if (completed != NULL) {
    ++(*completed);
    if (progress != NULL) {
      progress(*completed, total, user);
    }
  }
  return ok;
}

bool rmtree(const char *path) {
  if (!validate_rmtree_target(path)) {
    LOG_ERROR("Refusing unsafe recursive-delete path: %s",
              path ? path : "(null)");
    return false;
  }
  return rmtree_impl(path, NULL, NULL, NULL, 0);
}

bool rmtree_with_progress(const char *path, onion_fs_progress_fn progress,
                          void *user) {
  size_t completed = 0;
  size_t total;
  if (!validate_rmtree_target(path)) {
    LOG_ERROR("Refusing unsafe recursive-delete path: %s",
              path ? path : "(null)");
    return false;
  }
  if (!count_tree_entries(path, &total)) {
    return false;
  }
  if (progress != NULL) {
    progress(0, total, user);
  }
  return rmtree_impl(path, progress, user, &completed, total);
}
