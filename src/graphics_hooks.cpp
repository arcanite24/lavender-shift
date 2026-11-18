// Graphics settings that act on FH2's own code (docs/PINYON_PARITY.md,
// LP-2.1, LP-2.3, LP-2.5). Each site is Pinyon Shift's FH1 v4 hook mapped
// to TU v2 by its address mapper, with the nine instructions around it
// identical in both games. All of them take effect on the next frame.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/upscaler.h>

#include "graphics_hooks.h"
#include "guest_memory.h"
#include "render_settings.h"
#include "simulation_hooks.h"

REXCVAR_DEFINE_BOOL(disable_bloom, false, "Lavender Shift",
                    "Turn off the game's bloom (the bloom scale of each frame is zeroed; "
                    "weather, exposure and tone mapping stay as they are)");
REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "Lavender Shift",
                    "Turn off the game's motion blur (always off under a temporal upscaler)");
REXCVAR_DEFINE_DOUBLE(lavender_bloom_strength, 1.0, "Lavender Shift",
                      "Scale the game's bloom: 1 is the game's own (disable_bloom still turns "
                      "it off)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(lavender_motion_blur_strength, 1.0, "Lavender Shift",
                      "Scale the game's motion blur: 1 is the game's own (disable_motion_blur "
                      "still turns it off)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_BOOL(disable_depth_of_field, false, "Lavender Shift",
                    "Turn off the game's depth of field (always off under a temporal upscaler)");
REXCVAR_DEFINE_DOUBLE(lavender_fov_scale, 1.0, "Lavender Shift",
                      "Scale every camera's vertical field of view")
    .range(0.5, 2.0);
// Not saved: a hidden HUD at the next start would look like a bug.
REXCVAR_DEFINE_BOOL(hud_hidden, false, "Lavender Shift",
                    "Hide the game's HUD for clean screenshots; F2 flips it");

namespace {

// Calls of each hook since start, logged now and then with --lavender_log_hooks:
// how a mapped site is shown to run in FH2.
enum HookId { kBloom, kMotionBlur, kDepthOfField, kFieldOfView, kHookCount };
std::atomic<uint64_t> g_hook_calls[kHookCount];

void CountHook(HookId id);

}  // namespace

REXCVAR_DEFINE_BOOL(lavender_log_hooks, false, "Lavender Shift",
                    "Log how many times each settings hook has run, every 10 s");

// The frame's interpolated bloom scale, a float at r1+264, after the
// debug-mode adjustment (TU v2 0x82474E2C; FH1 v4 0x82494E44). BLOOM zeroes
// it or scales it.
void LavenderBloomScale(PPCRegister& r1) {
  CountHook(kBloom);
  const uint32_t address = r1.u32 + 264u;
  if (REXCVAR_GET(disable_bloom)) {
    lavender_shift::guest::StoreU32(address, 0u);
    return;
  }
  const double strength = REXCVAR_GET(lavender_bloom_strength);
  if (strength != 1.0) {
    lavender_shift::guest::StoreF32(address,
                                    float(lavender_shift::guest::LoadF32(address) * strength));
  }
}

// Motion blur's strength is stored once, while a scene loads, by one
// instruction (TU v2 0x82F0483C, stfs f8 to r3+80; FH1 v4 0x82E3A924, the
// Xenia Canary FH1 patch skips it). Skipping it leaves the strength at 0.
// The game never stores it again, so the hook keeps each object, the value
// the game stored and the value written in its place, and ApplyMotionBlur
// writes the stored value times MOTION BLUR (0 when off) back when the
// setting changes. Measured: the site runs 6 times while loading and never
// in play.
namespace {

struct BlurStrength {
  uint32_t object;
  float value;    // What the game stored.
  float written;  // What r3+80 holds now.
};
std::mutex g_blur_mutex;
std::vector<BlurStrength> g_blur_strengths;
double g_blur_applied_scale = 1.0;

// What the game's strength is multiplied by: 0 while motion blur is off.
double MotionBlurScale() {
  if (REXCVAR_GET(disable_motion_blur) || rex::ui::IsTemporalUpscalerActive()) return 0.0;
  return REXCVAR_GET(lavender_motion_blur_strength);
}

}  // namespace

// True skips the game's store: the hook has written the strength itself.
bool LavenderDisableMotionBlur(PPCRegister& r3, PPCRegister& f8) {
  CountHook(kMotionBlur);
  const double scale = MotionBlurScale();
  const float value = float(f8.f64);
  const BlurStrength strength = {r3.u32, value, float(value * scale)};
  std::lock_guard<std::mutex> lock(g_blur_mutex);
  auto it = std::find_if(g_blur_strengths.begin(), g_blur_strengths.end(),
                         [&](const BlurStrength& s) { return s.object == strength.object; });
  if (it != g_blur_strengths.end()) {
    *it = strength;
  } else {
    g_blur_strengths.push_back(strength);
  }
  g_blur_applied_scale = scale;
  if (scale == 1.0) return false;
  lavender_shift::guest::StoreF32(r3.u32 + 80u, strength.written);
  return true;
}

// Each of the three depth-of-field decisions reads 0 instead of its flag
// (TU v2 0x8246AC34, 0x82467C0C, 0x82467C3C; FH1 v4 0x8248AB7C, 0x82487B54,
// 0x82487B84).
bool LavenderDisableDepthOfField(PPCRegister& r11) {
  CountHook(kDepthOfField);
  if (!REXCVAR_GET(disable_depth_of_field) && !rex::ui::IsTemporalUpscalerActive()) {
    return false;
  }
  r11.u64 = 0;
  return true;
}

// The camera's vertical field of view in radians, returned in f1 by the
// getter whose return is at TU v2 0x829BB414 (FH1 v4 0x828FF264).
void LavenderCameraFieldOfView(PPCRegister& f1) {
  CountHook(kFieldOfView);
  const double scale = REXCVAR_GET(lavender_fov_scale);
  if (scale != 1.0) f1.f64 = double(float(f1.f64 * scale));
}

namespace {

// Each HUD element's visibility update hides the element while this byte is
// set (TU v2 0x83449CA2; FH1 v4 0x833F0F91). The toggle holds it at 1 while
// the HUD is hidden and restores the old value when it is shown.
constexpr uint32_t kHudHidden = 0x83449CA2u;
bool g_hud_applied = false;

void CountHook(HookId id) {
  g_hook_calls[id].fetch_add(1, std::memory_order_relaxed);
}
uint8_t g_hud_saved = 0;

}  // namespace

namespace lavender_shift::graphics {

void ToggleHud() {
  rex::cvar::SetFlagByName("hud_hidden", REXCVAR_GET(hud_hidden) ? "false" : "true");
}

void LogHookCalls() {
  if (!REXCVAR_GET(lavender_log_hooks)) return;
  static uint64_t steps = 0;
  if (++steps % 600) return;
  REXLOG_INFO("Hook calls: bloom {}, motion blur {}, depth of field {}, field of view {}; "
              "render scenarios {}, time of day {:.0f} s, render idle sleeps {}",
              g_hook_calls[kBloom].load(), g_hook_calls[kMotionBlur].load(),
              g_hook_calls[kDepthOfField].load(), g_hook_calls[kFieldOfView].load(),
              render_settings::ScenarioCount(), render_settings::SecondsOfDay(),
              simulation::RenderIdleCalls());
}

void ApplyMotionBlur() {
  const double scale = MotionBlurScale();
  std::lock_guard<std::mutex> lock(g_blur_mutex);
  if (scale == g_blur_applied_scale) return;
  g_blur_applied_scale = scale;
  size_t written = 0;
  for (BlurStrength& strength : g_blur_strengths) {
    // Only an object that still holds what was written: one freed with its
    // scene is left alone.
    if (guest::LoadF32(strength.object + 80u) != strength.written) continue;
    strength.written = float(strength.value * scale);
    guest::StoreF32(strength.object + 80u, strength.written);
    ++written;
  }
  REXLOG_INFO("Motion blur at {:.0f}% ({} of {} blur objects)", scale * 100.0, written,
              g_blur_strengths.size());
}

void UpdateHud() {
  const bool hidden = REXCVAR_GET(hud_hidden);
  if (hidden) {
    if (!g_hud_applied) g_hud_saved = guest::LoadU8(kHudHidden);
    guest::StoreU8(kHudHidden, 1);
  } else if (g_hud_applied) {
    guest::StoreU8(kHudHidden, g_hud_saved);
  }
  if (hidden != g_hud_applied) {
    g_hud_applied = hidden;
    REXLOG_INFO("HUD {}", hidden ? "hidden" : "shown");
  }
}

}  // namespace lavender_shift::graphics
