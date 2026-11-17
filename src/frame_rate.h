#pragma once

#include <cstdint>

namespace lavender_shift::frame_rate {

// Applies lavender_frame_rate now and whenever it, the monitor or fullscreen
// changes. `native_window` is the main window's HWND (its monitor is the one
// DISPLAY follows).
void Install(void* native_window);
void Uninstall();
void Apply();

// The refresh rate of the window's monitor, 24 to 240 Hz (60 if unknown).
uint32_t DisplayRefreshRate();

// Before boot: video_mode_refresh_rate (the guest video mode's refresh, which
// ShiftGlue halves for the game while it generates frames) follows the
// display, unless the command line set it. Pinyon Shift does the same.
void PublishRefreshRate(void* native_window);

}  // namespace lavender_shift::frame_rate
