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

static bool is_real_directory_path(const char *path) {
  char checked[1024];
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

  if (length >= sizeof(checked)) {
    errno = ENAMETOOLONG;
    return false;
  }
  memcpy(checked, path, length);
  checked[length] = '\0';

  /* Reject existing symlinks in any component before opendir follows them. */
  for (size_t i = 0; i <= length; ++i) {
    if (checked[i] != '/' && checked[i] != '\0')
      continue;
    if (i == 0 || checked[i - 1] == '/')
      continue;

    const char separator = checked[i];
    checked[i] = '\0';
    struct stat st;
    const int stat_result = lstat(checked, &st);
    checked[i] = separator;
    if (stat_result != 0)
      return false;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return false;
    }
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
  if (!path) {
    return false;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd > 0) {
    close(fd);
    return true;
  }
  return false;
}

bool write_file_atomic(const char *path, const void *data, size_t size) {
  char temp_path[1024];

  if (!path || path[0] == '\0' || (size > 0 && !data))
    return false;

  const int n =
      snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d", path, (int)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp_path))
    return false;

  unlink(temp_path);
  const int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
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
    if (current[0] != '\0' && mkdir(current, 0777) != 0 && errno != EEXIST) {
      return false;
    }
    current[i] = '/';
  }
  return mkdir(current, 0777) == 0 || errno == EEXIST;
}

static bool count_tree_entries(const char *path, size_t *out_count) {
  if (!path || !out_count) {
    errno = EINVAL;
    return false;
  }

  DIR *dir = opendir(path);
  size_t count = 1; /* The directory itself. */
  struct dirent *entry;
  bool success = true;

  if (dir == NULL) {
    LOG_ERROR("Error opening directory %s while counting: %s", path,
              strerror(errno));
    return false;
  }

  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) {
        LOG_ERROR("Error reading directory %s while counting: %s", path,
                  strerror(errno));
        success = false;
      }
      break;
    }

    char child[1024];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    const int child_length =
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (child_length < 0 || (size_t)child_length >= sizeof(child)) {
      LOG_ERROR("Path too long while counting %s/%s", path, entry->d_name);
      success = false;
      continue;
    }

    bool is_directory = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
      struct stat st;
      if (lstat(child, &st) != 0) {
        LOG_ERROR("Error inspecting %s while counting: %s", child,
                  strerror(errno));
        success = false;
        continue;
      }
      is_directory = S_ISDIR(st.st_mode);
    }

    size_t child_count = 1;
    if (is_directory && !count_tree_entries(child, &child_count)) {
      success = false;
      continue;
    }
    if (child_count > SIZE_MAX - count) {
      LOG_ERROR("Entry count overflow while counting %s", path);
      errno = EOVERFLOW;
      success = false;
      continue;
    }
    count += child_count;
  }

  if (closedir(dir) != 0) {
    LOG_ERROR("Error closing directory %s while counting: %s", path,
              strerror(errno));
    success = false;
  }
  if (!success) {
    return false;
  }

  *out_count = count;
  return true;
}

static bool rmtree_impl(const char *path, onion_fs_progress_fn progress,
                        void *user, size_t *completed, size_t total) {
  bool success = true;

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

  struct dirent *entry;
  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) {
        LOG_ERROR("Error reading directory %s: %s", path, strerror(errno));
        success = false;
      }
      break;
    }

    bool is_directory;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char child[1024];
    const int child_length =
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (child_length < 0 || (size_t)child_length >= sizeof(child)) {
      LOG_ERROR("Path too long while deleting %s/%s", path, entry->d_name);
      success = false;
      continue;
    }
    is_directory = is_directory_entry(child, entry->d_type);

    if (is_directory) {
      if (!rmtree_impl(child, progress, user, completed, total)) {
        success = false;
      }
    } else {
      if (unlink(child) != 0) {
        LOG_ERROR("Error deleting file %s: %s", child, strerror(errno));
        success = false;
      } else if (completed != NULL) {
        ++(*completed);
        if (progress != NULL) {
          progress(*completed, total, user);
        }
      }
    }
  }

  if (closedir(dir) != 0) {
    LOG_ERROR("Error closing directory %s: %s", path, strerror(errno));
    success = false;
  }

  if (rmdir(path) != 0) {
    LOG_ERROR("Error deleting folder %s: %s", path, strerror(errno));
    success = false;
  } else if (completed != NULL) {
    ++(*completed);
    if (progress != NULL) {
      progress(*completed, total, user);
    }
  }
  return success;
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
  if (!is_real_directory_path(path)) {
    LOG_ERROR("Path is not a directory: %s: %s", path, strerror(errno));
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
