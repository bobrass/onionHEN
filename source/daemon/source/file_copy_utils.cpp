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
