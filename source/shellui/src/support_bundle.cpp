#include "support_bundle.hpp"

#include <onion/fs.h>
#include <onion/version.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
constexpr size_t kTailLimit = 128 * 1024;
constexpr size_t kCopyBufferSize = 16 * 1024;
// ShellUI sees the console's /data tree through its /user mount namespace.
constexpr const char *kOutputDirectory = "/user/data/OnionHEN/support";
constexpr const char *kDisplayOutputDirectory = "/data/OnionHEN/support";

struct LogSource {
  const char *label;
  const char *display_path;
  const char *shellui_path;
};

constexpr LogSource kLogs[] = {
    {"Main log", "/data/OnionHEN/OnionHEN.log",
     "/user/data/OnionHEN/OnionHEN.log"},
    {"Main crash log", "/data/OnionHEN/OnionHEN_crash.log",
     "/user/data/OnionHEN/OnionHEN_crash.log"},
    {"Utility log", "/data/OnionHEN/OnionHEN_util_daemon.log",
     "/user/data/OnionHEN/OnionHEN_util_daemon.log"},
    {"Utility crash log", "/data/OnionHEN/OnionHEN_util_crash.log",
     "/user/data/OnionHEN/OnionHEN_util_crash.log"},
};

bool write_all(int fd, const void *data, size_t size) {
  const char *bytes = static_cast<const char *>(data);
  while (size > 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return false;
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool write_text(int fd, const std::string &text) {
  return write_all(fd, text.data(), text.size());
}

bool append_log(int output_fd, const LogSource &log) {
  if (!write_text(output_fd, "\n\n===== " + std::string(log.label) +
                                  " (" + log.display_path + ") =====\n"))
    return false;

  const int input_fd = open(log.shellui_path, O_RDONLY);
  if (input_fd < 0) {
    return write_text(output_fd, "[unavailable: could not open]\n");
  }

  struct stat info {};
  if (fstat(input_fd, &info) != 0 || info.st_size < 0) {
    close(input_fd);
    return write_text(output_fd, "[unavailable: could not inspect]\n");
  }

  const off_t start = info.st_size > static_cast<off_t>(kTailLimit)
                          ? info.st_size - static_cast<off_t>(kTailLimit)
                          : 0;
  if (lseek(input_fd, start, SEEK_SET) < 0) {
    close(input_fd);
    return write_text(output_fd, "[unavailable: could not seek]\n");
  }
  if (start > 0 && !write_text(output_fd, "[earlier log content omitted]\n")) {
    close(input_fd);
    return false;
  }

  char buffer[kCopyBufferSize];
  size_t copied = 0;
  while (copied < kTailLimit) {
    const size_t remaining = kTailLimit - copied;
    const size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const ssize_t count = read(input_fd, buffer, request);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      close(input_fd);
      return false;
    }
    if (count == 0)
      break;
    if (!write_all(output_fd, buffer, static_cast<size_t>(count))) {
      close(input_fd);
      return false;
    }
    copied += static_cast<size_t>(count);
  }

  const bool closed = close(input_fd) == 0;
  if (!closed)
    return false;
  return write_text(output_fd, "\n");
}
} // namespace

bool create_support_bundle(std::string &output_path) {
  if (!mkdir_tree(kOutputDirectory))
    return false;

  const std::time_t now = std::time(nullptr);
  for (unsigned int suffix = 0; suffix < 100; ++suffix) {
    char path[160];
    const int length = std::snprintf(
        path, sizeof(path), "%s/OnionHEN-support-%lld-%ld-%u.txt",
        kOutputDirectory, static_cast<long long>(now),
        static_cast<long>(getpid()), suffix);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(path))
      return false;

    const int output_fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output_fd < 0) {
      if (errno == EEXIST)
        continue;
      return false;
    }

    const std::string header =
        "OnionHEN local support bundle\nBuild: " ONIONHEN_VERSION
        "\nGenerated (Unix time): " + std::to_string(now) +
        "\nEach available log is limited to its last 128 KiB.\n"
        "This file may contain title IDs, network addresses, and file paths. "
        "Inspect it before sharing.\n"
        "Configuration, cheats, and user payloads are not included. "
        "This file was not uploaded.\n";
    bool ok = write_text(output_fd, header);
    for (const LogSource &log : kLogs) {
      if (ok)
        ok = append_log(output_fd, log);
    }
    if (close(output_fd) != 0)
      ok = false;
    if (!ok) {
      unlink(path);
      return false;
    }
    output_path = kDisplayOutputDirectory;
    output_path += path + std::strlen(kOutputDirectory);
    return true;
  }
  return false;
}
