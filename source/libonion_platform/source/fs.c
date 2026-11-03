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

static size_t count_tree_entries(const char *path) {
  size_t count = 1; /* The directory itself. */
  struct dirent *entry;
  struct stat path_stat;

  if (lstat(path, &path_stat) != 0 || !S_ISDIR(path_stat.st_mode))
    return count;

  DIR *dir = opendir(path);

  if (dir == NULL) {
    LOG_WARN("Could not open directory while counting %s: %s", path,
             strerror(errno));
    return count;
  }
  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno == EINTR)
        continue;
      if (errno != 0)
        LOG_WARN("Could not finish counting directory %s: %s", path,
                 strerror(errno));
      break;
    }
    char child[1024];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    const int child_length =
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (child_length < 0 || (size_t)child_length >= sizeof(child)) {
      LOG_WARN("Skipping overlong path while counting %s/%s", path,
               entry->d_name);
      continue;
    }

    struct stat child_stat;
    if (lstat(child, &child_stat) != 0) {
      LOG_WARN("Could not stat path while counting %s: %s", child,
               strerror(errno));
      continue;
    }
    const size_t child_count = S_ISDIR(child_stat.st_mode)
                                   ? count_tree_entries(child)
                                   : 1;
    if (child_count > SIZE_MAX - count) {
      count = SIZE_MAX;
      LOG_WARN("Directory entry count saturated while counting %s", path);
    } else {
      count += child_count;
    }
  }
  if (closedir(dir) != 0)
    LOG_WARN("Could not close directory while counting %s: %s", path,
             strerror(errno));
  return count;
}

static void remember_delete_error(int *first_error, int error) {
  if (*first_error == 0)
    *first_error = error != 0 ? error : EIO;
}

static void report_delete_progress(onion_fs_progress_fn progress, void *user,
                                   size_t *completed, size_t total) {
  if (completed == NULL)
    return;
  ++(*completed);
  if (progress != NULL)
    progress(*completed, total, user);
}

static bool rmtree_impl(const char *path, onion_fs_progress_fn progress,
                        void *user, size_t *completed, size_t total) {
  if (!path) {
    return false;
  }

  struct stat path_stat;
  if (lstat(path, &path_stat) != 0) {
    const int stat_error = errno;
    LOG_ERROR("Error inspecting path %s: %s", path, strerror(stat_error));
    errno = stat_error;
    return false;
  }
  if (S_ISLNK(path_stat.st_mode)) {
    if (unlink(path) != 0) {
      const int unlink_error = errno;
      LOG_ERROR("Error deleting symbolic link %s: %s", path,
                strerror(unlink_error));
      errno = unlink_error;
      return false;
    }
    report_delete_progress(progress, user, completed, total);
    return true;
  }
  if (!S_ISDIR(path_stat.st_mode)) {
    errno = ENOTDIR;
    LOG_ERROR("Refusing to recursively delete non-directory path %s", path);
    return false;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    const int open_error = errno;
    LOG_ERROR("Error opening directory %s: %s", path, strerror(open_error));
    errno = open_error;
    return false;
  }

  struct stat opened_stat;
  errno = 0;
  if (fstat(dirfd(dir), &opened_stat) != 0 ||
      opened_stat.st_dev != path_stat.st_dev ||
      opened_stat.st_ino != path_stat.st_ino) {
    const int identity_error = errno != 0 ? errno : EAGAIN;
    closedir(dir);
    LOG_ERROR("Directory changed while preparing to delete %s: %s", path,
              strerror(identity_error));
    errno = identity_error;
    return false;
  }

  bool success = true;
  int first_error = 0;
  struct dirent *entry;
  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno == EINTR)
        continue;
      if (errno != 0) {
        success = false;
        remember_delete_error(&first_error, errno);
        LOG_ERROR("Error reading directory %s: %s", path, strerror(errno));
      }
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char child[1024];
    const int child_length =
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (child_length < 0 || (size_t)child_length >= sizeof(child)) {
      success = false;
      remember_delete_error(&first_error, ENAMETOOLONG);
      LOG_ERROR("Path is too long while deleting %s/%s", path, entry->d_name);
      continue;
    }

    struct stat child_stat;
    if (lstat(child, &child_stat) != 0) {
      success = false;
      remember_delete_error(&first_error, errno);
      LOG_ERROR("Error inspecting path %s: %s", child, strerror(errno));
      continue;
    }
    if (S_ISDIR(child_stat.st_mode)) {
      if (!rmtree_impl(child, progress, user, completed, total)) {
        success = false;
        remember_delete_error(&first_error, errno);
      }
    } else if (unlink(child) != 0) {
      success = false;
      remember_delete_error(&first_error, errno);
      LOG_ERROR("Error deleting path %s: %s", child, strerror(errno));
    } else {
      report_delete_progress(progress, user, completed, total);
    }
  }

  errno = 0;
  if (closedir(dir) != 0) {
    success = false;
    remember_delete_error(&first_error, errno);
    LOG_ERROR("Error closing directory %s: %s", path, strerror(errno));
  }

  if (rmdir(path) != 0) {
    success = false;
    remember_delete_error(&first_error, errno);
    LOG_ERROR("Error deleting directory %s: %s", path, strerror(errno));
  } else {
    report_delete_progress(progress, user, completed, total);
  }
  if (!success) {
    errno = first_error;
    return false;
  }
  return true;
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
  total = count_tree_entries(path);
  if (progress != NULL) {
    progress(0, total, user);
  }
  return rmtree_impl(path, progress, user, &completed, total);
}
