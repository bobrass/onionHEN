/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Generic recursive copy / size helpers for daemon IPC (BREW_COPY_*).
 */

#include <onion/platform.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct DirectoryIdentityStack {
  dev_t devices[PATH_MAX];
  ino_t inodes[PATH_MAX];
};

static bool push_directory_identity(DIR *dir, DirectoryIdentityStack *stack,
                                    size_t depth) {
  if (depth >= static_cast<size_t>(PATH_MAX)) {
    errno = ELOOP;
    return false;
  }

  struct stat info;
  if (fstat(dirfd(dir), &info) != 0)
    return false;
  for (size_t index = 0; index < depth; ++index) {
    if (stack->devices[index] == info.st_dev &&
        stack->inodes[index] == info.st_ino) {
      errno = ELOOP;
      return false;
    }
  }
  stack->devices[depth] = info.st_dev;
  stack->inodes[depth] = info.st_ino;
  return true;
}

static void add_size_saturating(uint64_t *total, uint64_t amount,
                               const char *path) {
  if (*total == UINT64_MAX)
    return;
  if (amount > UINT64_MAX - *total) {
    *total = UINT64_MAX;
    LOG_WARN("directory size exceeded uint64_t range while scanning %s", path);
    return;
  }
  *total += amount;
}

static uint64_t calculate_total_size(const char *path,
                                    DirectoryIdentityStack *stack,
                                    size_t depth) {
  uint64_t total = 0;
  DIR *dir = opendir(path);
  if (dir == NULL) {
    onion_notify(false, "notify.fs.size_failed", path);
    return 0;
  }
  if (!push_directory_identity(dir, stack, depth)) {
    const int identity_error = errno;
    closedir(dir);
    LOG_WARN("skipping recursive directory while calculating size %s: %s",
             path, strerror(identity_error));
    return 0;
  }

  struct dirent *entry;
  char fullPath[1024];
  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno == EINTR)
        continue;
      if (errno != 0)
        LOG_WARN("directory size scan incomplete for %s: %s", path,
                 strerror(errno));
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    const int path_length =
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
    if (path_length < 0 ||
        static_cast<size_t>(path_length) >= sizeof(fullPath)) {
      LOG_WARN("directory size scan skipped overlong path: %s/%s", path,
               entry->d_name);
      continue;
    }
    struct stat st;
    if (stat(fullPath, &st) != 0) {
      LOG_WARN("directory size scan cannot stat %s: %s", fullPath,
               strerror(errno));
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      add_size_saturating(
          &total, calculate_total_size(fullPath, stack, depth + 1), path);
    } else if (S_ISREG(st.st_mode) && st.st_size >= 0) {
      add_size_saturating(&total, static_cast<uint64_t>(st.st_size), path);
    }
  }

  if (closedir(dir) != 0)
    LOG_WARN("directory size scan could not close %s: %s", path,
             strerror(errno));
  return total;
}

uint64_t calculateTotalSize(const char *path) {
  DirectoryIdentityStack stack = {};
  return calculate_total_size(path, &stack, 0);
}

static const char *sizes[] = {"EiB", "PiB", "TiB", "GiB", "MiB", "KiB", "B"};
static const uint64_t exbibytes =
    1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;

#define DIM(x) (sizeof(x) / sizeof(x[0]))

void calculateSize(uint64_t size, char *result) {
  uint64_t multiplier = exbibytes;
  for (int i = 0; i < static_cast<int>(DIM(sizes)); i++, multiplier /= 1024) {
    if (size < multiplier)
      continue;
    if (size % multiplier == 0)
      sprintf(result, "%lu %s", size / multiplier, sizes[i]);
    else
      sprintf(result, "%.1f %s", static_cast<float>(size) / multiplier, sizes[i]);
    return;
  }
  strcpy(result, "0");
}

bool copyFile(const char *source, const char *destination) {
  FILE *src = fopen(source, "rb");
  if (src == NULL) {
    const int open_error = errno;
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile failed to open %s: %s", source,
              strerror(open_error));
    errno = open_error;
    return false;
  }

  struct stat source_stat;
  struct stat destination_stat;
  if (fstat(fileno(src), &source_stat) != 0) {
    const int stat_error = errno;
    fclose(src);
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile failed to inspect %s: %s", source,
              strerror(stat_error));
    errno = stat_error;
    return false;
  }

  errno = 0;
  const int destination_stat_result = stat(destination, &destination_stat);
  if (destination_stat_result == 0 &&
      source_stat.st_dev == destination_stat.st_dev &&
      source_stat.st_ino == destination_stat.st_ino) {
    fclose(src);
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile source and destination refer to the same file: %s",
              source);
    errno = EINVAL;
    return false;
  }
  if (destination_stat_result != 0 && errno != ENOENT) {
    const int stat_error = errno;
    fclose(src);
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile failed to inspect %s: %s", destination,
              strerror(stat_error));
    errno = stat_error;
    return false;
  }

  FILE *dest = fopen(destination, "wb");
  if (dest == NULL) {
    const int open_error = errno;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile failed to open %s: %s", destination,
              strerror(open_error));
    fclose(src);
    errno = open_error;
    return false;
  }

  char buffer[1024];
  bool success = true;
  int copy_error = 0;
  for (;;) {
    errno = 0;
    const size_t bytes = fread(buffer, 1, sizeof(buffer), src);
    const int read_error = ferror(src) ? (errno != 0 ? errno : EIO) : 0;
    if (bytes > 0) {
      errno = 0;
      if (fwrite(buffer, 1, bytes, dest) != bytes) {
        success = false;
        copy_error = errno != 0 ? errno : EIO;
        break;
      }
    }
    if (read_error != 0) {
      success = false;
      copy_error = read_error;
      break;
    }
    if (bytes < sizeof(buffer))
      break;
  }

  errno = 0;
  const int source_close_result = fclose(src);
  if (source_close_result != 0 && success) {
    success = false;
    copy_error = errno != 0 ? errno : EIO;
  }
  errno = 0;
  const int destination_close_result = fclose(dest);
  if (destination_close_result != 0 && success) {
    success = false;
    copy_error = errno != 0 ? errno : EIO;
  }

  if (success)
    return true;

  if (unlink(destination) != 0 && errno != ENOENT)
    LOG_WARN("failed to remove incomplete copy %s: %s", destination,
             strerror(errno));
  errno = copy_error;
  onion_notify(false, "notify.fs.copy_file", source);
  LOG_ERROR("copyFile failed for %s -> %s: %s", source, destination,
            strerror(copy_error));
  return false;
}

static bool is_same_or_child_path(const char *parent, const char *candidate) {
  const size_t parent_length = strlen(parent);
  if (strncmp(parent, candidate, parent_length) != 0)
    return false;
  if (parent_length == 1 && parent[0] == '/')
    return candidate[0] == '/';
  return candidate[parent_length] == '\0' ||
         candidate[parent_length] == '/';
}

static bool copy_recursive_impl(const char *source, const char *destination,
                                DirectoryIdentityStack *stack, size_t depth) {
  DIR *dir = opendir(source);
  if (dir == NULL) {
    const int open_error = errno;
    onion_notify(false, "notify.fs.copy_recursive", source);
    LOG_ERROR("copyRecursive failed to open %s: %s", source,
              strerror(open_error));
    errno = open_error;
    return false;
  }
  if (!push_directory_identity(dir, stack, depth)) {
    const int identity_error = errno;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", source);
    LOG_ERROR("copyRecursive detected a directory cycle at %s: %s", source,
              strerror(identity_error));
    errno = identity_error;
    return false;
  }

  const bool destination_created = mkdir(destination, 0777) == 0;
  if (!destination_created && errno != EEXIST) {
    const int mkdir_error = errno;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    LOG_ERROR("copyRecursive failed to create %s: %s", destination,
              strerror(mkdir_error));
    errno = mkdir_error;
    return false;
  }

  char resolved_source[PATH_MAX];
  char resolved_destination[PATH_MAX];
  if (realpath(source, resolved_source) == NULL ||
      realpath(destination, resolved_destination) == NULL) {
    const int resolve_error = errno != 0 ? errno : EIO;
    if (destination_created && rmdir(destination) != 0)
      LOG_WARN("copyRecursive failed to remove destination %s: %s",
               destination, strerror(errno));
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    LOG_ERROR("copyRecursive failed to resolve source/destination paths: %s",
              strerror(resolve_error));
    errno = resolve_error;
    return false;
  }
  if (is_same_or_child_path(resolved_source, resolved_destination)) {
    if (destination_created && rmdir(resolved_destination) != 0)
      LOG_WARN("copyRecursive failed to remove recursive destination %s: %s",
               resolved_destination, strerror(errno));
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    LOG_ERROR("copyRecursive destination cannot be inside its source: %s",
              destination);
    errno = EINVAL;
    return false;
  }

  struct stat destination_stat;
  errno = 0;
  const int destination_stat_result = stat(destination, &destination_stat);
  if (destination_stat_result != 0 || !S_ISDIR(destination_stat.st_mode)) {
    const int destination_error =
        destination_stat_result != 0 ? errno : ENOTDIR;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    LOG_ERROR("copyRecursive destination is not a directory: %s",
              destination);
    errno = destination_error;
    return false;
  }

  bool success = true;
  int copy_error = 0;
  struct dirent *entry;
  char srcPath[1024];
  char destPath[1024];
  for (;;) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
      if (errno == EINTR)
        continue;
      if (errno != 0) {
        success = false;
        if (copy_error == 0)
          copy_error = errno;
        LOG_ERROR("copyRecursive failed reading %s: %s", source,
                  strerror(errno));
      }
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    const int src_path_length =
        snprintf(srcPath, sizeof(srcPath), "%s/%s", source, entry->d_name);
    const int dest_path_length = snprintf(destPath, sizeof(destPath), "%s/%s",
                                          destination, entry->d_name);
    if (src_path_length < 0 ||
        static_cast<size_t>(src_path_length) >= sizeof(srcPath) ||
        dest_path_length < 0 ||
        static_cast<size_t>(dest_path_length) >= sizeof(destPath)) {
      success = false;
      if (copy_error == 0)
        copy_error = ENAMETOOLONG;
      LOG_ERROR("copyRecursive path is too long: %s/%s", source,
                entry->d_name);
      continue;
    }

    struct stat st;
    if (stat(srcPath, &st) != 0) {
      const int stat_error = errno != 0 ? errno : EIO;
      success = false;
      if (copy_error == 0)
        copy_error = stat_error;
      LOG_ERROR("copyRecursive failed to stat %s: %s", srcPath,
                strerror(stat_error));
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!copy_recursive_impl(srcPath, destPath, stack, depth + 1)) {
        success = false;
        if (copy_error == 0)
          copy_error = errno != 0 ? errno : EIO;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (!copyFile(srcPath, destPath)) {
        success = false;
        if (copy_error == 0)
          copy_error = errno != 0 ? errno : EIO;
      }
    }
  }

  errno = 0;
  if (closedir(dir) != 0 && success) {
    success = false;
    copy_error = errno != 0 ? errno : EIO;
    LOG_ERROR("copyRecursive failed closing %s: %s", source,
              strerror(copy_error));
  }

  if (success)
    return true;

  onion_notify(false, "notify.fs.copy_recursive", source);
  errno = copy_error;
  return false;
}

bool copyRecursive(const char *source, const char *destination) {
  DirectoryIdentityStack stack = {};
  return copy_recursive_impl(source, destination, &stack, 0);
}
