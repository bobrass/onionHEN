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
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return false;
}

bool write_file_atomic(const char *path, const void *data, size_t size) {
  char temp_path[1024];
  int fd = -1;

  if (!path || path[0] == '\0' || (size > 0 && !data))
    return false;

  /* Never follow a stale symlink or truncate another writer's staging file. */
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

static bool build_child_path(char *child, size_t child_size,
                             const char *path, const char *name) {
  const int written = snprintf(child, child_size, "%s/%s", path, name);
  return written >= 0 && (size_t)written < child_size;
}

static bool count_tree_entries(const char *path, size_t *count) {
  DIR *dir = opendir(path);
  size_t total = 1; /* The directory itself. */
  struct dirent *entry;
  int read_error = 0;

  if (dir == NULL) {
    return false;
  }
  for (;;) {
    char child[1024];
    size_t child_count = 1;
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      read_error = errno;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!build_child_path(child, sizeof(child), path, entry->d_name)) {
      closedir(dir);
      return false;
    }
    if (is_directory_entry(child, entry->d_type) &&
        !count_tree_entries(child, &child_count)) {
      closedir(dir);
      return false;
    }
    if (child_count > SIZE_MAX - total) {
      closedir(dir);
      return false;
    }
    total += child_count;
  }
  if (closedir(dir) != 0 || read_error != 0) {
    return false;
  }
  *count = total;
  return true;
}

static bool rmtree_impl(const char *path, onion_fs_progress_fn progress,
                        void *user, size_t *completed, size_t total) {
  if (!path) {
    return false;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    LOG_ERROR("Error opening directory %s", path);
    return false;
  }

  bool ok = true;
  struct dirent *entry;
  for (;;) {
    bool is_directory;
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno != 0) {
        LOG_ERROR("Error reading directory %s", path);
        ok = false;
      }
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char child[1024];
    if (!build_child_path(child, sizeof(child), path, entry->d_name)) {
      LOG_ERROR("Child path under %s is too long", path);
      ok = false;
      continue;
    }
    is_directory = is_directory_entry(child, entry->d_type);

    if (is_directory) {
      if (!rmtree_impl(child, progress, user, completed, total)) {
        ok = false;
      }
    } else if (unlink(child) != 0) {
      LOG_ERROR("Error deleting file %s", child);
      ok = false;
    } else if (completed != NULL) {
      ++(*completed);
      if (progress != NULL) {
        progress(*completed, total, user);
      }
    }
  }

  if (closedir(dir) != 0) {
    LOG_ERROR("Error closing directory %s", path);
    ok = false;
  }

  if (rmdir(path) != 0) {
    LOG_ERROR("Error deleting folder %s", path);
    ok = false;
  } else {
    if (completed != NULL) {
      ++(*completed);
      if (progress != NULL) {
        progress(*completed, total, user);
      }
    }
  }
  return ok;
}

bool rmtree(const char *path) {
  return rmtree_impl(path, NULL, NULL, NULL, 0);
}

bool rmtree_with_progress(const char *path, onion_fs_progress_fn progress,
                          void *user) {
  size_t completed = 0;
  size_t total;
  if (path == NULL) {
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
