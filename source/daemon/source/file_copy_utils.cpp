/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Generic recursive copy / size helpers for daemon IPC (BREW_COPY_*).
 */

#include <onion/platform.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

uint64_t calculateTotalSize(const char *path) {
  long total = 0;
  DIR *dir = opendir(path);
  if (dir == NULL) {
    onion_notify(false, "notify.fs.size_failed", path);
    return 0;
  }

  struct dirent *entry;
  char fullPath[1024];
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
    struct stat st;
    if (stat(fullPath, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      total += static_cast<long>(calculateTotalSize(fullPath));
    else if (S_ISREG(st.st_mode))
      total += st.st_size;
  }

  closedir(dir);
  return static_cast<uint64_t>(total);
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
  if (fstat(fileno(src), &source_stat) == 0 &&
      stat(destination, &destination_stat) == 0 &&
      source_stat.st_dev == destination_stat.st_dev &&
      source_stat.st_ino == destination_stat.st_ino) {
    fclose(src);
    errno = EINVAL;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile source and destination refer to the same file: %s",
              source);
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

bool copyRecursive(const char *source, const char *destination) {
  DIR *dir = opendir(source);
  if (dir == NULL) {
    onion_notify(false, "notify.fs.copy_recursive", source);
    return false;
  }

  mkdir(destination, 0777);

  struct dirent *entry;
  char srcPath[1024];
  char destPath[1024];
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    snprintf(srcPath, sizeof(srcPath), "%s/%s", source, entry->d_name);
    snprintf(destPath, sizeof(destPath), "%s/%s", destination, entry->d_name);

    struct stat st;
    if (stat(srcPath, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      copyRecursive(srcPath, destPath);
    } else if (S_ISREG(st.st_mode)) {
      if (!copyFile(srcPath, destPath)) {
        onion_notify(false, "notify.fs.copy_recursive", srcPath);
        closedir(dir);
        return false;
      }
    }
  }

  closedir(dir);
  return true;
}
