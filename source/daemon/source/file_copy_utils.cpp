/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Generic recursive copy / size helpers for daemon IPC (BREW_COPY_*).
 */

#include <onion/platform.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

struct SizeDirectoryAncestor {
  dev_t device;
  ino_t inode;
  const SizeDirectoryAncestor *parent;
};

static bool calculateTotalSizeInternal(const char *path, uint64_t *total,
                                       const SizeDirectoryAncestor *ancestors) {
  DIR *dir = opendir(path);
  if (dir == NULL) return false;

  struct stat directory_stat;
  if (fstat(dirfd(dir), &directory_stat) != 0) {
    const int error = errno;
    closedir(dir);
    errno = error;
    return false;
  }
  for (const SizeDirectoryAncestor *ancestor = ancestors; ancestor != NULL;
       ancestor = ancestor->parent) {
    if (directory_stat.st_dev == ancestor->device &&
        directory_stat.st_ino == ancestor->inode) {
      closedir(dir);
      errno = ELOOP;
      return false;
    }
  }
  const SizeDirectoryAncestor current_ancestor = {
      directory_stat.st_dev, directory_stat.st_ino, ancestors};

  uint64_t directory_total = 0;
  char fullPath[1024];
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
      int error = errno;
      if (closedir(dir) != 0 && error == 0) error = errno;
      if (error != 0) {
        errno = error;
        return false;
      }
      *total = directory_total;
      return true;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    const int path_length = snprintf(fullPath, sizeof(fullPath), "%s/%s", path,
                                     entry->d_name);
    if (path_length < 0 || static_cast<size_t>(path_length) >= sizeof(fullPath)) {
      closedir(dir);
      errno = ENAMETOOLONG;
      return false;
    }

    struct stat st;
    if (stat(fullPath, &st) != 0) {
      const int error = errno;
      closedir(dir);
      errno = error;
      return false;
    }

    uint64_t entry_size = 0;
    if (S_ISDIR(st.st_mode)) {
      if (!calculateTotalSizeInternal(fullPath, &entry_size,
                                      &current_ancestor)) {
        const int error = errno;
        closedir(dir);
        errno = error;
        return false;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (st.st_size < 0) {
        closedir(dir);
        errno = EOVERFLOW;
        return false;
      }
      entry_size = static_cast<uint64_t>(st.st_size);
    }

    if (UINT64_MAX - directory_total < entry_size) {
      closedir(dir);
      errno = EOVERFLOW;
      return false;
    }
    directory_total += entry_size;
  }
}

bool calculateTotalSize(const char *path, uint64_t *total) {
  if (total == NULL) {
    errno = EINVAL;
    onion_notify(false, "notify.fs.size_failed", path);
    errno = EINVAL;
    return false;
  }
  uint64_t result = 0;
  if (!calculateTotalSizeInternal(path, &result, NULL)) {
    const int error = errno;
    onion_notify(false, "notify.fs.size_failed", path);
    errno = error;
    return false;
  }
  *total = result;
  return true;
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
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile failed for %s", source);
    return false;
  }

  struct stat source_stat;
  if (fstat(fileno(src), &source_stat) != 0) {
    const int error = errno;
    fclose(src);
    errno = error;
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile failed to inspect %s: %s", source, strerror(errno));
    return false;
  }

  struct stat destination_stat;
  const int destination_stat_result = stat(destination, &destination_stat);
  if (destination_stat_result == 0 &&
      source_stat.st_dev == destination_stat.st_dev &&
      source_stat.st_ino == destination_stat.st_ino) {
    fclose(src);
    errno = EINVAL;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile source and destination are the same file: %s",
              source);
    return false;
  }
  if (destination_stat_result != 0 && errno != ENOENT) {
    const int error = errno;
    fclose(src);
    errno = error;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile failed to inspect %s: %s", destination,
              strerror(errno));
    return false;
  }

  FILE *dest = fopen(destination, "wb");
  if (dest == NULL) {
    const int error = errno;
    onion_notify(false, "notify.fs.copy_file", destination);
    fclose(src);
    errno = error;
    LOG_ERROR("copyFile failed for %s: %s", destination, strerror(errno));
    return false;
  }

  char buffer[1024];
  int error = 0;
  for (;;) {
    const size_t bytes = fread(buffer, 1, sizeof(buffer), src);
    if (bytes > 0 && fwrite(buffer, 1, bytes, dest) != bytes) {
      error = errno != 0 ? errno : EIO;
      break;
    }
    if (bytes < sizeof(buffer)) {
      if (ferror(src)) error = errno != 0 ? errno : EIO;
      break;
    }
  }

  if (fclose(src) != 0 && error == 0) error = errno != 0 ? errno : EIO;
  if (fclose(dest) != 0 && error == 0) error = errno != 0 ? errno : EIO;
  if (error != 0) {
    errno = error;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile failed writing %s: %s", destination,
              strerror(errno));
    return false;
  }
  return true;
}

struct CopyDirectoryAncestor {
  dev_t device;
  ino_t inode;
  const CopyDirectoryAncestor *parent;
};

static bool copyRecursiveInternal(const char *source, const char *destination,
                                  const CopyDirectoryAncestor *ancestors) {
  DIR *dir = opendir(source);
  if (dir == NULL) {
    onion_notify(false, "notify.fs.copy_recursive", source);
    return false;
  }

  struct stat source_stat;
  if (fstat(dirfd(dir), &source_stat) != 0) {
    const int error = errno;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", source);
    errno = error;
    return false;
  }
  for (const CopyDirectoryAncestor *ancestor = ancestors; ancestor != NULL;
       ancestor = ancestor->parent) {
    if (source_stat.st_dev == ancestor->device &&
        source_stat.st_ino == ancestor->inode) {
      closedir(dir);
      onion_notify(false, "notify.fs.copy_recursive", source);
      errno = ELOOP;
      return false;
    }
  }
  const CopyDirectoryAncestor current_ancestor = {
      source_stat.st_dev, source_stat.st_ino, ancestors};

  const bool created_destination = mkdir(destination, 0777) == 0;
  if (!created_destination && errno != EEXIST) {
    const int error = errno;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    errno = error;
    return false;
  }
  struct stat destination_stat;
  const int destination_stat_result = stat(destination, &destination_stat);
  if (destination_stat_result != 0 ||
      !S_ISDIR(destination_stat.st_mode)) {
    const int error = destination_stat_result != 0 ? errno : ENOTDIR;
    closedir(dir);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    errno = error;
    return false;
  }

  char resolved_source[PATH_MAX];
  char resolved_destination[PATH_MAX];
  if (realpath(source, resolved_source) == NULL ||
      realpath(destination, resolved_destination) == NULL) {
    const int error = errno;
    closedir(dir);
    if (created_destination) rmdir(destination);
    onion_notify(false, "notify.fs.copy_recursive", source);
    errno = error;
    return false;
  }

  const size_t source_length = strlen(resolved_source);
  const bool destination_is_source =
      strcmp(resolved_source, resolved_destination) == 0;
  const bool destination_is_descendant =
      strncmp(resolved_source, resolved_destination, source_length) == 0 &&
      (source_length == 1 || resolved_destination[source_length] == '/');
  if (destination_is_source || destination_is_descendant) {
    const int error = EINVAL;
    closedir(dir);
    if (created_destination) rmdir(destination);
    onion_notify(false, "notify.fs.copy_recursive", destination);
    errno = error;
    return false;
  }

  char srcPath[1024];
  char destPath[1024];
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
      int error = errno;
      if (closedir(dir) != 0 && error == 0) error = errno;
      if (error != 0) {
        onion_notify(false, "notify.fs.copy_recursive", source);
        errno = error;
        return false;
      }
      return true;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    const int src_length =
        snprintf(srcPath, sizeof(srcPath), "%s/%s", source, entry->d_name);
    const int dest_length = snprintf(destPath, sizeof(destPath), "%s/%s",
                                     destination, entry->d_name);
    if (src_length < 0 || static_cast<size_t>(src_length) >= sizeof(srcPath) ||
        dest_length < 0 ||
        static_cast<size_t>(dest_length) >= sizeof(destPath)) {
      closedir(dir);
      onion_notify(false, "notify.fs.copy_recursive", source);
      errno = ENAMETOOLONG;
      return false;
    }

    struct stat st;
    if (stat(srcPath, &st) != 0) {
      const int error = errno;
      closedir(dir);
      onion_notify(false, "notify.fs.copy_recursive", srcPath);
      errno = error;
      return false;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!copyRecursiveInternal(srcPath, destPath, &current_ancestor)) {
        const int error = errno;
        closedir(dir);
        errno = error;
        return false;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (!copyFile(srcPath, destPath)) {
        const int error = errno;
        onion_notify(false, "notify.fs.copy_recursive", srcPath);
        closedir(dir);
        errno = error;
        return false;
      }
    }
  }
}

bool copyRecursive(const char *source, const char *destination) {
  return copyRecursiveInternal(source, destination, NULL);
}
