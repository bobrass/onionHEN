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
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>

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
  if (!source || !*source || !destination || !*destination) {
    errno = EINVAL;
    onion_notify(false, "notify.fs.copy_file", "");
    return false;
  }

  FILE *src = fopen(source, "rb");
  if (src == NULL) {
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile failed for %s", source);
    return false;
  }

  struct stat sourceStat;
  struct stat destinationStat;
  mode_t destinationMode = 0666;
  bool destinationExists = false;
  if (fstat(fileno(src), &sourceStat) != 0) {
    const int savedErrno = errno;
    fclose(src);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", source);
    LOG_ERROR("copyFile could not stat source %s", source);
    return false;
  }
  if (lstat(destination, &destinationStat) == 0) {
    if (!S_ISREG(destinationStat.st_mode) ||
        (sourceStat.st_dev == destinationStat.st_dev &&
         sourceStat.st_ino == destinationStat.st_ino)) {
      fclose(src);
      errno = EINVAL;
      onion_notify(false, "notify.fs.copy_file", destination);
      LOG_ERROR("copyFile rejected destination %s", destination);
      return false;
    }
    destinationExists = true;
    destinationMode = destinationStat.st_mode & 0777;
  } else if (errno != ENOENT) {
    const int savedErrno = errno;
    fclose(src);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile could not inspect destination %s: %s", destination,
              strerror(errno));
    return false;
  }

  char tempPath[PATH_MAX];
  int tempFd = -1;
  for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
    const int written = snprintf(tempPath, sizeof(tempPath), "%s.tmp.%d.%u",
                                 destination, static_cast<int>(getpid()),
                                 attempt);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(tempPath)) {
      fclose(src);
      errno = ENAMETOOLONG;
      onion_notify(false, "notify.fs.copy_file", destination);
      return false;
    }
    do {
      tempFd = open(tempPath, O_WRONLY | O_CREAT | O_EXCL, destinationMode);
    } while (tempFd < 0 && errno == EINTR);
    if (tempFd >= 0 || errno != EEXIST)
      break;
  }
  if (tempFd < 0) {
    const int savedErrno = errno;
    fclose(src);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile could not create staging file for %s: %s", destination,
              strerror(errno));
    return false;
  }
  if (destinationExists && fchmod(tempFd, destinationMode) != 0) {
    const int savedErrno = errno;
    close(tempFd);
    unlink(tempPath);
    fclose(src);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    return false;
  }

  FILE *dest = fdopen(tempFd, "wb");
  if (!dest) {
    const int savedErrno = errno;
    close(tempFd);
    unlink(tempPath);
    fclose(src);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    return false;
  }

  char buffer[1024];
  int copyErrno = 0;
  size_t bytes = 0;
  errno = 0;
  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes, dest) != bytes) {
      copyErrno = errno != 0 ? errno : EIO;
      break;
    }
  }
  if (copyErrno == 0 && ferror(src))
    copyErrno = errno != 0 ? errno : EIO;

  if (fclose(src) != 0 && copyErrno == 0)
    copyErrno = errno != 0 ? errno : EIO;
  if (copyErrno == 0 && fflush(dest) != 0)
    copyErrno = errno != 0 ? errno : EIO;
  if (copyErrno == 0 && fsync(fileno(dest)) != 0)
    copyErrno = errno != 0 ? errno : EIO;
  if (fclose(dest) != 0 && copyErrno == 0)
    copyErrno = errno != 0 ? errno : EIO;

  if (copyErrno != 0) {
    unlink(tempPath);
    errno = copyErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile failed while copying %s to %s: %s", source,
              destination, strerror(errno));
    return false;
  }

  if (rename(tempPath, destination) != 0) {
    const int savedErrno = errno;
    unlink(tempPath);
    errno = savedErrno;
    onion_notify(false, "notify.fs.copy_file", destination);
    LOG_ERROR("copyFile could not publish %s: %s", destination,
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
