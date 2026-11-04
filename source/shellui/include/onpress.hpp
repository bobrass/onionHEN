/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Table-driven OnPress dispatch for ShellUI toolbox items.
 */

#pragma once

#include "shellui_types.hpp"
#include "hooked_funcs.hpp"
#include "ipc.hpp"
#include <onion/platform.h>
#include <onion/proc_query.h>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <string>

/** Per-press context shared by domain handlers. */
struct OnPressContext {
  MonoObject *instance = nullptr;
  MonoObject *element = nullptr;
  MonoObject *event = nullptr;
  std::string id;
  std::string value;
  std::string title;

  /** Request settings_commit after successful handling. */
  bool reload_main = false;
  bool reload_util = false;
  bool dirty = true; // SaveSettings by default when handled
};

/**
 * Handler result:
 * - Handled: domain handled the id; dispatcher will settings_commit (if dirty) then oOnPress.
 * - Consumed: fully handled custom/dynamic id; optional commit if dirty, do NOT call oOnPress.
 *   Use for ids outside the stock Settings model (e.g. id_cheat_*), where original
 *   SettingPage.OnPressed may null-deref on unknown elements.
 * - EarlyReturn: stop now; call oOnPress without commit (no-op / validation fail).
 * - NotMine: try next matcher / fall through.
 */
enum class OnPressResult {
  Handled,
  Consumed,
  EarlyReturn,
  NotMine,
};

using OnPressHandler = OnPressResult (*)(OnPressContext &ctx);

struct OnPressExactEntry {
  const char *id;
  OnPressHandler handler;
};

struct OnPressPrefixEntry {
  const char *prefix;
  OnPressHandler handler;
};

/* Domain tables (defined in onpress_*.cpp). */
const OnPressExactEntry *onpress_overlay_exact(size_t *count);
const OnPressExactEntry *onpress_network_exact(size_t *count);
const OnPressExactEntry *onpress_system_exact(size_t *count);
const OnPressExactEntry *onpress_misc_root_exact(size_t *count);
const OnPressExactEntry *onpress_account_exact(size_t *count);

const OnPressPrefixEntry *onpress_payloads_prefix(size_t *count);
const OnPressPrefixEntry *onpress_cheats_prefix(size_t *count);
const OnPressPrefixEntry *onpress_sprx_prefix(size_t *count);

const OnPressExactEntry *onpress_plugins_exact(size_t *count);
const OnPressPrefixEntry *onpress_plugins_prefix(size_t *count);
const OnPressExactEntry *onpress_pkg_installer_exact(size_t *count);
const OnPressPrefixEntry *onpress_pkg_installer_prefix(size_t *count);

/**
 * Shared built-in plugin handlers (root page + plugins page).
 * Run toggles change the current session only. Autoload toggles persist for
 * the next OnionHEN start and do not start or stop the service now.
 */
OnPressResult onpress_kstuff_autoload(OnPressContext &ctx);
OnPressResult onpress_delete_kstuff(OnPressContext &ctx);

/** Parse a complete, in-range decimal value from a ShellUI list or field. */
inline bool parse_onpress_int(const OnPressContext &ctx, int *value) {
  if (!value) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long parsed = std::strtol(ctx.value.c_str(), &end, 10);
  if (errno == ERANGE || end == ctx.value.c_str()) {
    return false;
  }
  while (*end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != 0 || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

/** Parse the explicit 0/1 value used by ShellUI toggle controls. */
inline bool parse_onpress_bool(const OnPressContext &ctx, bool *value) {
  if (!value) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long parsed = std::strtol(ctx.value.c_str(), &end, 10);
  if (errno == ERANGE || end == ctx.value.c_str()) {
    return false;
  }
  while (*end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0' || (parsed != 0 && parsed != 1)) {
    return false;
  }
  *value = parsed != 0;
  return true;
}
