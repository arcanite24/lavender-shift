// The one frame rate setting (docs/PINYON_PARITY.md LP-1.14). FH2 renders a
// frame every two guest vblanks, so the rate it simulates and renders at is
// the SDK's render limit (pinyon_shift_fh1_render_fps_limit, vblanks at twice
// it). lavender_frame_rate owns that limit: a number, DISPLAY (the refresh
// rate of the monitor the window is on) or UNLOCKED (as fast as the PC
// goes). The host's own present limit stays off, so no second limiter drops
// frames the game rendered; vsync only decides whether presents wait for the
// display. FH2 keeps real time at any of these rates: the simulation advances
// by measured time (120.0 steps and 1.000 s a second at 120, 240.0 at 240),
// and the constant-step code is scaled (src/fixed_steps.cpp).

#include "frame_rate.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "diagnostics.h"

REXCVAR_DEFINE_STRING(lavender_frame_rate, "display", "Display",
                      "Frame rate the game runs at: a number (30 to 240), display (the "
                      "monitor's refresh rate) or unlocked")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::frame_rate {
namespace {

// UNLOCKED: guest vblanks at 2 kHz, far above what a frame takes.
constexpr uint32_t kUnlockedLimit = 1000;
constexpr uint32_t kFallbackRefresh = 60;

void* g_window = nullptr;
uint32_t g_applied = 0;

void SetUnlessCommandLine(const char* name, const std::string& value) {
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine) return;
  if (rex::cvar::GetFlagByName(name) != value) rex::cvar::SetFlagByName(name, value);
}

}  // namespace

uint32_t DisplayRefreshRate() {
#if defined(_WIN32)
  const HWND window = static_cast<HWND>(g_window);
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (monitor && GetMonitorInfoW(monitor, &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    return std::clamp<uint32_t>(mode.dmDisplayFrequency, 24, 240);
  }
#endif
  // Elsewhere (Android) the display's rate is not read yet: 60.
  return kFallbackRefresh;
}

void Apply() {
  const std::string setting = REXCVAR_GET(lavender_frame_rate);
  uint32_t limit = 0;
  if (setting == "unlocked") {
    limit = kUnlockedLimit;
  } else if (setting == "display" || setting.empty()) {
    limit = DisplayRefreshRate();
  } else {
    limit = std::clamp(uint32_t(std::max(0, std::atoi(setting.c_str()))), 24u, 240u);
  }
  SetUnlessCommandLine("pinyon_shift_fh1_render_fps_limit", std::to_string(limit));
  SetUnlessCommandLine("host_present_fps_limit", "0");
  if (limit != g_applied) {
    g_applied = limit;
    diagnostics::RecordEvent("display.frame_rate",
                             {{"setting", setting}, {"render_limit", std::to_string(limit)}});
  }
}

void Install(void* native_window) {
  g_window = native_window;
  Apply();
  rex::cvar::RegisterChangeCallback("lavender_frame_rate",
                                    [](std::string_view, std::string_view) { Apply(); });
  // The SDK listens to these too, so they are never unregistered by name:
  // after Uninstall the callbacks do nothing.
  static bool registered = false;
  if (!registered) {
    registered = true;
    for (const char* name : {"monitor", "fullscreen"}) {
      rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
        if (g_window) Apply();
      });
    }
  }
}

void Uninstall() {
  rex::cvar::UnregisterChangeCallbacks("lavender_frame_rate");
  g_window = nullptr;
}

}  // namespace lavender_shift::frame_rate
