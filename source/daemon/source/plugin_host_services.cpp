#include "plugin_host_services.hpp"

#include <onion/log.h>
#include <onion/notify.h>
#include <onion/plugin_manager.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace onion::daemon::plugin_host {
namespace {

constexpr size_t kMaxConfigFile = 64u * 1024u;

enum class FileReadStatus { Ok, Missing, Error };

bool valid_key(std::string_view key) {
  if (key.empty() || key.size() >= 64) return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char c) {
    return c >= 0x21 && c <= 0x7e && c != '=';
  });
}

bool valid_value(std::string_view value) {
  if (value.size() >= 4096) return false;
  return value.find('\n') == std::string_view::npos &&
         value.find('\r') == std::string_view::npos;
}

FileReadStatus read_file(const std::string &path, std::string &out) {
  out.clear();
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return errno == ENOENT ? FileReadStatus::Missing : FileReadStatus::Error;
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      const int savedErrno = errno;
      close(fd);
      out.clear();
      errno = savedErrno;
      return FileReadStatus::Error;
    }
    if (count == 0)
      break;
    if (out.size() > kMaxConfigFile - static_cast<size_t>(count)) {
      close(fd);
      out.clear();
      errno = EFBIG;
      return FileReadStatus::Error;
    }
    out.append(buffer, static_cast<size_t>(count));
  }
  if (close(fd) != 0) {
    const int savedErrno = errno;
    out.clear();
    errno = savedErrno;
    return FileReadStatus::Error;
  }
  return FileReadStatus::Ok;
}

bool ensure_parent_directory(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return true;
  const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
  if (mkdir(parent.c_str(), 0777) != 0 && errno != EEXIST)
    return false;
  struct stat entry {};
  if (lstat(parent.c_str(), &entry) != 0)
    return false;
  if (!S_ISDIR(entry.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  return true;
}

bool write_file(const std::string &path, const std::string &body) {
  if (!ensure_parent_directory(path))
    return false;
  const std::string temporary_path = path + ".tmp";
#ifdef O_NOFOLLOW
  const int fd = open(temporary_path.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
#else
  const int fd = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                      0666);
#endif
  if (fd < 0)
    return false;
  size_t offset = 0;
  while (offset < body.size()) {
    const ssize_t count =
        write(fd, body.data() + offset, body.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      const int savedErrno = count == 0 ? EIO : errno;
      close(fd);
      unlink(temporary_path.c_str());
      errno = savedErrno;
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  if (fsync(fd) != 0) {
    const int savedErrno = errno;
    close(fd);
    unlink(temporary_path.c_str());
    errno = savedErrno;
    return false;
  }
  if (close(fd) != 0) {
    const int savedErrno = errno;
    unlink(temporary_path.c_str());
    errno = savedErrno;
    return false;
  }
  if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
    const int savedErrno = errno;
    unlink(temporary_path.c_str());
    errno = savedErrno;
    return false;
  }
  return true;
}

plugin_ui::Status file_error_status(int error) {
  return error == EACCES || error == EPERM
             ? plugin_ui::Status::PermissionDenied
             : plugin_ui::Status::InvalidState;
}

} // namespace

ConfigStore::ConfigStore(std::string root) : root_(std::move(root)) {}

std::string ConfigStore::path_for(std::string_view owner) const {
  return root_ + "/" + std::string(owner) + ".cfg";
}

bool ConfigStore::load(std::string_view owner) {
  const std::string owner_str(owner);
  std::map<std::string, std::string> values;
  std::string body;
  const FileReadStatus read_status = read_file(path_for(owner), body);
  if (read_status == FileReadStatus::Error)
    return false;
  if (read_status == FileReadStatus::Ok) {
    size_t line_start = 0;
    while (line_start <= body.size()) {
      const size_t line_end = body.find('\n', line_start);
      const std::string_view line(
          body.data() + line_start,
          (line_end == std::string::npos ? body.size() : line_end) -
              line_start);
      const size_t eq = line.find('=');
      if (eq != std::string_view::npos) {
        const std::string key(line.substr(0, eq));
        if (!key.empty())
          values.emplace(key, std::string(line.substr(eq + 1)));
      }
      if (line_end == std::string::npos) break;
      line_start = line_end + 1;
    }
  }
  cache_[owner_str] = std::move(values);
  return true;
}

bool ConfigStore::save(std::string_view owner) {
  const auto found = cache_.find(std::string(owner));
  if (found == cache_.end()) return false;
  std::string body;
  for (const auto &[key, value] : found->second) {
    body += key;
    body += '=';
    body += value;
    body += '\n';
  }
  if (!write_file(path_for(owner), body)) return false;
  return true;
}

plugin_ui::Status ConfigStore::get(std::string_view owner,
                                   std::string_view key,
                                   std::vector<uint8_t> &value) {
  value.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string owner_str(owner);
  if (!cache_.contains(owner_str) && !load(owner))
    return file_error_status(errno);
  const auto found = cache_.find(owner_str);
  if (found == cache_.end()) return plugin_ui::Status::NotFound;
  const auto kv = found->second.find(std::string(key));
  if (kv == found->second.end()) return plugin_ui::Status::NotFound;
  value.assign(kv->second.begin(), kv->second.end());
  return plugin_ui::Status::Ok;
}

plugin_ui::Status ConfigStore::set(std::string_view owner,
                                   std::string_view key,
                                   std::string_view value) {
  if (!valid_key(key) || !valid_value(value))
    return plugin_ui::Status::InvalidArgument;
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string owner_str(owner);
  if (!cache_.contains(owner_str) && !load(owner))
    return file_error_status(errno);
  auto &values = cache_[owner_str];
  const std::string key_str(key);
  const auto old_value = values.find(key_str);
  const bool had_old_value = old_value != values.end();
  const std::string previous_value = had_old_value ? old_value->second : "";
  values[key_str] = std::string(value);
  if (!save(owner)) {
    const int savedErrno = errno;
    if (had_old_value)
      values[key_str] = previous_value;
    else
      values.erase(key_str);
    LOG_WARN("[plugins] config persist failed for %s", owner_str.c_str());
    errno = savedErrno;
    return file_error_status(savedErrno);
  }
  return plugin_ui::Status::Ok;
}

PluginHostServices::PluginHostServices(ConfigStore &store) : store_(store) {}

plugin_ui::Status PluginHostServices::log(std::string_view owner,
                                          uint32_t level,
                                          std::string_view message) {
  std::string line;
  line.reserve(owner.size() + message.size() + 4);
  line += '[';
  line.append(owner);
  line += "] ";
  line.append(message);
  switch (level) {
  case 0:
    LOG_DEBUG("%s", line.c_str());
    break;
  case 1:
    LOG_INFO("%s", line.c_str());
    break;
  case 2:
    LOG_WARN("%s", line.c_str());
    break;
  default:
    LOG_ERROR("%s", line.c_str());
    break;
  }
  return plugin_ui::Status::Ok;
}

plugin_ui::Status PluginHostServices::notify(std::string_view owner,
                                             std::string_view message) {
  (void)owner;
  onion_notify(false, "%.*s", static_cast<int>(message.size()),
               message.data());
  return plugin_ui::Status::Ok;
}

plugin_ui::Status PluginHostServices::config_get(std::string_view owner,
                                                 std::string_view key,
                                                 std::vector<uint8_t> &value) {
  return store_.get(owner, key, value);
}

plugin_ui::Status PluginHostServices::config_set(std::string_view owner,
                                                 std::string_view key,
                                                 std::string_view value) {
  return store_.set(owner, key, value);
}

PluginHostServices &host_services() {
  static ConfigStore store(plugin::kInstallRoot + std::string("/config"));
  static PluginHostServices services(store);
  return services;
}

} // namespace onion::daemon::plugin_host
