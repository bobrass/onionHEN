/* Copyright (C) 2025 OnionHEN / LightningMods */
#include "onpress.hpp"
#include "pkg_installer.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <system_error>

namespace {

bool parse_index(const std::string &id, const char *prefix, std::size_t *out) {
  const std::string start(prefix);
  if (id.rfind(start, 0) != 0 || id.size() == start.size())
    return false;
  char *end = nullptr;
  const unsigned long value = std::strtoul(id.c_str() + start.size(), &end, 10);
  if (!end || *end != '\0' || value > 100000)
    return false;
  *out = static_cast<std::size_t>(value);
  return true;
}

bool parse_install_selection(const std::string &id, std::uint64_t *generation,
                             std::size_t *index) {
  constexpr char prefix[] = "id_pkg_install_";
  constexpr std::size_t prefix_size = sizeof(prefix) - 1;
  if (id.rfind(prefix, 0) != 0)
    return false;

  const std::size_t separator = id.find('_', prefix_size);
  if (separator == std::string::npos || separator == prefix_size ||
      separator + 1 == id.size())
    return false;

  std::uint64_t parsed_generation = 0;
  std::size_t parsed_index = 0;
  const char *generation_begin = id.data() + prefix_size;
  const char *generation_end = id.data() + separator;
  const char *index_begin = generation_end + 1;
  const char *selection_end = id.data() + id.size();
  const auto generation_result =
      std::from_chars(generation_begin, generation_end, parsed_generation);
  const auto index_result =
      std::from_chars(index_begin, selection_end, parsed_index);
  if (generation_result.ec != std::errc{} ||
      generation_result.ptr != generation_end ||
      index_result.ec != std::errc{} || index_result.ptr != selection_end ||
      parsed_index > 100000)
    return false;

  *generation = parsed_generation;
  *index = parsed_index;
  return true;
}

std::string trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c));
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c));
  }).base();
  return first < last ? std::string(first, last) : std::string();
}

OnPressResult path_input(OnPressContext &ctx) {
  set_pkg_search_path_draft(ctx.value);
  ctx.dirty = false;
  return OnPressResult::Consumed;
}

OnPressResult add_path(OnPressContext &ctx) {
  ctx.dirty = false;
  const std::string path = trim(pkg_search_path_draft());
  if (!pkg_search_path_is_valid(path)) {
    notify("notify.pkg.path_invalid");
    return OnPressResult::Consumed;
  }
  for (std::size_t i = 0; i < g_settings.custom_pkg_search_path_count; ++i) {
    if (g_settings.custom_pkg_search_paths[i] == path) {
      notify("notify.pkg.path_duplicate");
      return OnPressResult::Consumed;
    }
  }
  if (g_settings.custom_pkg_search_path_count >=
      onion::kMaxCustomPkgSearchPaths) {
    notify("notify.pkg.path_limit");
    return OnPressResult::Consumed;
  }
  g_settings.custom_pkg_search_paths[g_settings.custom_pkg_search_path_count++] =
      path;
  ctx.dirty = true;
  notify("notify.pkg.path_added");
  return OnPressResult::Consumed;
}

OnPressResult remove_path(OnPressContext &ctx) {
  std::size_t index = 0;
  if (!parse_index(ctx.id, "id_pkg_remove_", &index) ||
      index >= g_settings.custom_pkg_search_path_count) {
    ctx.dirty = false;
    return OnPressResult::Consumed;
  }
  for (std::size_t i = index + 1;
       i < g_settings.custom_pkg_search_path_count; ++i)
    g_settings.custom_pkg_search_paths[i - 1] =
        std::move(g_settings.custom_pkg_search_paths[i]);
  --g_settings.custom_pkg_search_path_count;
  g_settings.custom_pkg_search_paths[g_settings.custom_pkg_search_path_count]
      .clear();
  ctx.dirty = true;
  notify("notify.pkg.path_removed");
  return OnPressResult::Consumed;
}

OnPressResult install_pkg(OnPressContext &ctx) {
  ctx.dirty = false;
  std::uint64_t generation = 0;
  std::size_t index = 0;
  if (!parse_install_selection(ctx.id, &generation, &index)) {
    notify("notify.pkg.install_failed", -EINVAL);
    return OnPressResult::Consumed;
  }
  const PkgInstallerResult result = install_scanned_pkg(
      generation, index, g_settings.custom_pkg_search_paths.data(),
      g_settings.custom_pkg_search_path_count);
  if (result.started)
    notify("notify.pkg.install_started");
  else
    notify("notify.pkg.install_failed", result.error_code);
  return OnPressResult::Consumed;
}

const OnPressExactEntry kExact[] = {
    {"id_pkg_path_input", path_input},
    {"id_pkg_path_add", add_path},
};

const OnPressPrefixEntry kPrefix[] = {
    {"id_pkg_remove_", remove_path},
    {"id_pkg_install_", install_pkg},
};

} // namespace

const OnPressExactEntry *onpress_pkg_installer_exact(size_t *count) {
  *count = sizeof(kExact) / sizeof(kExact[0]);
  return kExact;
}

const OnPressPrefixEntry *onpress_pkg_installer_prefix(size_t *count) {
  *count = sizeof(kPrefix) / sizeof(kPrefix[0]);
  return kPrefix;
}
