/* Copyright (C) 2025 OnionHEN / LightningMods — OnPress overlay domain */
#include "onpress.hpp"
#include <cstdlib>
#include <unistd.h>

void RemoveGameWidget(RemoveWidget widget);
bool CreateGameWidget(CreateWidget widget);

/** Tear down all segments, recompute horizontal packing, rebuild enabled ones. */
static void rebuild_overlay_bar() {
  RemoveGameWidget(REMOVE_ALL_OVERLAYS);
  apply_overlay_layout();
  if (!g_settings.overlay_enabled)
    return;
  if (g_settings.overlay_fps)
    CreateGameWidget(CREATE_FPS_OVERLAY);
  if (g_settings.overlay_cpu || g_settings.all_cpu_usage)
    CreateGameWidget(CREATE_CPU_OVERLAY);
  if (g_settings.overlay_gpu)
    CreateGameWidget(CREATE_GPU_OVERLAY);
  if (g_settings.overlay_ram)
    CreateGameWidget(CREATE_RAM_OVERLAY);
  if (g_settings.overlay_ip)
    CreateGameWidget(CREATE_IP_OVERLAY);
  if (g_settings.overlay_fan)
    CreateGameWidget(CREATE_FAN_OVERLAY);
}

static OnPressResult toggle_overlay_flag(OnPressContext &ctx, bool &flag) {
  bool enabled = false;
  if (!parse_onpress_bool(ctx, &enabled)) {
    LOG_WARN("Rejected overlay toggle value for %s: %s", ctx.id.c_str(),
             ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (enabled == flag) {
    return OnPressResult::EarlyReturn;
  }
  flag = enabled;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_enabled(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_enabled);
}

static OnPressResult id_overlay_background(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_background);
}

static OnPressResult id_overlay_gpu(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_gpu);
}

static OnPressResult id_overlay_cpu(OnPressContext &ctx) {
  bool enabled = false;
  if (!parse_onpress_bool(ctx, &enabled)) {
    LOG_WARN("Rejected CPU overlay state: %s", ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (enabled == g_settings.overlay_cpu) {
    return OnPressResult::EarlyReturn;
  }
  if (!enabled && g_settings.all_cpu_usage) {
    notify("notify.overlay.disable_cpu_first");
    return OnPressResult::EarlyReturn;
  }
  g_settings.overlay_cpu = enabled;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_fps(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_fps);
}

static OnPressResult id_overlay_ram(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_ram);
}

static OnPressResult id_overlay_ip(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_ip);
}

static OnPressResult id_overlay_fan(OnPressContext &ctx) {
  return toggle_overlay_flag(ctx, g_settings.overlay_fan);
}

static OnPressResult id_all_cpu_usage(OnPressContext &ctx) {
  bool enabled = false;
  if (!parse_onpress_bool(ctx, &enabled)) {
    LOG_WARN("Rejected all CPU usage state: %s", ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (g_settings.all_cpu_usage == enabled) {
    return OnPressResult::EarlyReturn;
  }
  if (!g_settings.overlay_cpu) {
    notify("notify.overlay.enable_cpu_first");
    return OnPressResult::EarlyReturn;
  }
  /* Persisted via settings_commit as overlay.cpu_usage_mode in config.ini. */
  g_settings.all_cpu_usage = enabled;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_change_pos(OnPressContext &ctx) {
  int position = 0;
  if (!parse_onpress_int(ctx, &position) ||
      (position != OVERLAY_POS_TOP_LEFT &&
       position != OVERLAY_POS_BOTTOM_LEFT)) {
    LOG_WARN("Rejected overlay position: %s", ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (position == g_settings.overlay_pos) {
    return OnPressResult::EarlyReturn;
  }
  g_settings.overlay_pos = position;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_align(OnPressContext &ctx) {
  int align = 0;
  if (!parse_onpress_int(ctx, &align) || align < 0 || align > 2) {
    LOG_WARN("Rejected overlay alignment: %s", ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (align == g_settings.overlay_align) {
    return OnPressResult::EarlyReturn;
  }
  g_settings.overlay_align = align;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_font_size(OnPressContext &ctx) {
  int size = 0;
  if (!parse_onpress_int(ctx, &size) ||
      (size != onion::kOverlayFontSmall &&
       size != onion::kOverlayFontMedium &&
       size != onion::kOverlayFontLarge)) {
    LOG_WARN("Rejected overlay font size: %s", ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  if (size == g_settings.overlay_font_size) {
    return OnPressResult::EarlyReturn;
  }

  g_settings.overlay_font_size = size;
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult overlay_set_order(OnPressContext &ctx, int metric) {
  int position = 0;
  if (!parse_onpress_int(ctx, &position) || position < 1 ||
      position > onion::kOverlayMetricCount) {
    LOG_WARN("Rejected overlay metric position for %s: %s", ctx.id.c_str(),
             ctx.value.c_str());
    return OnPressResult::EarlyReturn;
  }
  const int current =
      onion::overlay_metric_position(g_settings.overlay_order, metric);
  if (position == current) {
    return OnPressResult::EarlyReturn;
  }

  onion::overlay_move_metric(g_settings.overlay_order, metric, position);
  rebuild_overlay_bar();
  ctx.reload_main = true;
  return OnPressResult::Handled;
}

static OnPressResult id_overlay_fps_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricFps);
}

static OnPressResult id_overlay_cpu_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricCpu);
}

static OnPressResult id_overlay_gpu_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricGpu);
}

static OnPressResult id_overlay_ram_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricMemory);
}

static OnPressResult id_overlay_ip_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricIp);
}

static OnPressResult id_overlay_fan_order(OnPressContext &ctx) {
  return overlay_set_order(ctx, onion::kOverlayMetricFan);
}

static const OnPressExactEntry kExact[] = {
    {"id_overlay_enabled", id_overlay_enabled},
    {"id_overlay_background", id_overlay_background},
    {"id_overlay_gpu", id_overlay_gpu},
    {"id_overlay_cpu", id_overlay_cpu},
    {"id_overlay_fps", id_overlay_fps},
    {"id_overlay_ram", id_overlay_ram},
    {"id_overlay_ip", id_overlay_ip},
    {"id_overlay_fan", id_overlay_fan},
    {"id_all_cpu_usage", id_all_cpu_usage},
    {"id_overlay_change_pos", id_overlay_change_pos},
    {"id_overlay_align", id_overlay_align},
    {"id_overlay_font_size", id_overlay_font_size},
    {"id_overlay_fps_order", id_overlay_fps_order},
    {"id_overlay_cpu_order", id_overlay_cpu_order},
    {"id_overlay_gpu_order", id_overlay_gpu_order},
    {"id_overlay_ram_order", id_overlay_ram_order},
    {"id_overlay_ip_order", id_overlay_ip_order},
    {"id_overlay_fan_order", id_overlay_fan_order},
};

const OnPressExactEntry *onpress_overlay_exact(size_t *count) {
  *count = sizeof(kExact) / sizeof(kExact[0]);
  return kExact;
}
