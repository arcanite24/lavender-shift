// Shadows and reflection updates (docs/PINYON_PARITY.md LP-2.2), through
// FH2's render scenarios, as Pinyon Shift does for FH1.
//
// Each render scenario's XML sets DynamicRenderSettings controls. A hook
// after each control loads (TU v2 0x82F0D058, r30 the control, r26 the
// scenario; FH1 v4 0x82E43458, exact) records the gameplay scenarios by
// their EnvMapFrequencyScale, the one float control (vtable TU v2
// 0x8224B4B4, found from FH1's by the functions it points to; its value at
// +4). Then, once per simulation step:
// - the reflection rate scales each scenario's EnvMapFrequencyScale from
//   the value its XML set (how often the dynamic cube map cars reflect is
//   redrawn);
// - shadows off sets each scenario's SkipShadowMapUnlessCockpit (a bool
//   control at +0x194, vtable TU v2 0x8224B584; its set bit is bit 3 of
//   the word at +40) and keeps the screen's shadow mask lit, as the
//   executor no longer resolves it. In FH1 that switch also dropped the
//   headlights' light on the road, so shadows go off only by day.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/thread/mutex.h>

#include "guest_memory.h"
#include "render_settings.h"

REXCVAR_DEFINE_DOUBLE(lavender_env_map_rate, 1.0, "Lavender Shift",
                      "Scale how often the dynamic cube map cars reflect is redrawn "
                      "(1: the game's rate, 0: never)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(lavender_shadows, true, "Lavender Shift",
                    "Draw the sun's shadows. Off applies only by day, while the cars' "
                    "lights are off, and keeps the screen's shadow mask lit");

namespace {

constexpr uint32_t kFloatControlVtable = 0x8224B4B4u;
constexpr uint32_t kBoolControlVtable = 0x8224B584u;
constexpr uint32_t kSkipShadowOffset = 0x194, kSetBitsOffset = 40, kSkipShadowBit = 1u << 3;
// The world, as last passed to its time-of-day accessor (TU v2 0x82496DA0,
// lwz r3,232(r3); FH1 v4 0x824B6990, the same code). world+232 is the time
// of day, its +10488 the seconds since midnight (read by the same
// instructions as FH1's). FH1's world-holder path (holder+4, +4) does not
// lead to the world in FH2.
std::atomic<uint32_t> g_world{0};

struct RenderScenario {
  uint32_t address;
  uint32_t env_control;
  float env_value;
  uint8_t skip_value;
  bool skip_set;
};

std::mutex g_mutex;
std::vector<RenderScenario> g_scenarios;
float g_applied_env_rate = 1.0f;
bool g_shadows_skipped = false;
uint32_t g_mask_resolves_seen = 0;
uint32_t g_mask_resolves_filled = UINT32_MAX;

namespace guest = lavender_shift::guest;

bool HasSkipShadowControl(uint32_t scenario) {
  return guest::LoadU32(scenario + kSkipShadowOffset) == kBoolControlVtable;
}

void SetShadowsSkipped(const RenderScenario& scenario, bool skipped) {
  if (!HasSkipShadowControl(scenario.address)) return;
  guest::StoreU8(scenario.address + kSkipShadowOffset + 4, skipped ? 1 : scenario.skip_value);
  uint32_t bits = guest::LoadU32(scenario.address + kSetBitsOffset) & ~kSkipShadowBit;
  if (skipped || scenario.skip_set) bits |= kSkipShadowBit;
  guest::StoreU32(scenario.address + kSetBitsOffset, bits);
}

void SetEnvMapRate(const RenderScenario& scenario, float rate) {
  if (guest::LoadU32(scenario.env_control) != kFloatControlVtable) return;
  guest::StoreF32(scenario.env_control + 4, scenario.env_value * rate);
}

// Seconds since midnight in game, or -1 when there is no world.
double GameSecondsOfDay() {
  auto follow = [](uint32_t address, uint32_t offset) -> uint32_t {
    if (!guest::Readable(address + offset, 4)) return 0;
    return guest::LoadU32(address + offset);
  };
  const uint32_t world = g_world.load(std::memory_order_relaxed);
  const uint32_t time_of_day = world ? follow(world, 232) : 0;
  if (!time_of_day || !guest::Readable(time_of_day + 10488, 4)) return -1.0;
  const float seconds = guest::LoadF32(time_of_day + 10488);
  return std::isfinite(seconds) && seconds >= 0.0f && seconds <= 86400.0f ? seconds : -1.0;
}

// FH1's cars switch their lights on outside 08:02-18:50 (its SwitchOnLights
// curve); FH2's curve is not read yet, so the same hours are used.
bool CarLightsOn() {
  const double seconds = GameSecondsOfDay();
  return seconds >= 0.0 && (seconds < 482.0 * 60.0 || seconds > 1130.0 * 60.0);
}

// Called with g_mutex held.
void KeepShadowsOff() {
  auto* kernel = rex::system::kernel_state();
  auto* graphics = kernel && kernel->emulator() ? kernel->emulator()->graphics_system() : nullptr;
  uint32_t mask_base, mask_length, mask_resolves;
  if (!graphics || !graphics->fh1_shadow_mask(&mask_base, &mask_length, &mask_resolves)) return;
  if (!g_shadows_skipped) {
    for (const RenderScenario& scenario : g_scenarios) SetShadowsSkipped(scenario, true);
    g_shadows_skipped = true;
    REXLOG_INFO("Shadows off: {} render scenarios, shadow mask {:08X}+{:X}", g_scenarios.size(),
                mask_base, mask_length);
  }
  // Without the shadow passes the mask keeps whatever was last there: fill
  // it lit (00 FF FF 00, the two shadow terms full) whenever a frame goes by
  // without the game resolving it (the cockpit view still draws shadows).
  const bool resolving = mask_resolves != g_mask_resolves_seen;
  g_mask_resolves_seen = mask_resolves;
  if (resolving) return;
  auto* memory = kernel->memory();
  auto* mask = memory->TranslatePhysical<uint32_t*>(mask_base);
  constexpr uint32_t kLitMask = 0x00FFFF00u;
  if (mask_resolves == g_mask_resolves_filled && mask[0] == kLitMask &&
      mask[mask_length / 4 - 1] == kLitMask) {
    return;
  }
  g_mask_resolves_filled = mask_resolves;
  std::fill(mask, mask + mask_length / 4, kLitMask);
  memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                         0xA0000000u + mask_base, mask_length, true, false);
}

}  // namespace

void LavenderObserveWorld(PPCRegister& r3) { g_world.store(r3.u32, std::memory_order_relaxed); }

void LavenderRenderSettingLoaded(PPCRegister& r26, PPCRegister& r30) {
  if (guest::LoadU32(r30.u32) != kFloatControlVtable) return;
  RenderScenario scenario = {r26.u32, r30.u32, guest::LoadF32(r30.u32 + 4), 0, false};
  if (HasSkipShadowControl(r26.u32)) {
    scenario.skip_value = guest::LoadU8(r26.u32 + kSkipShadowOffset + 4);
    scenario.skip_set = (guest::LoadU32(r26.u32 + kSetBitsOffset) & kSkipShadowBit) != 0;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_scenarios.empty()) {
    g_applied_env_rate = float(REXCVAR_GET(lavender_env_map_rate));
  }
  // A scenario reloaded at the same address replaces its old record.
  std::erase_if(g_scenarios, [&](const RenderScenario& s) { return s.address == scenario.address; });
  g_scenarios.push_back(scenario);
  SetEnvMapRate(scenario, g_applied_env_rate);
  if (g_shadows_skipped) SetShadowsSkipped(scenario, true);
}

namespace lavender_shift::render_settings {

void Apply() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_scenarios.empty()) return;
  const float rate = float(REXCVAR_GET(lavender_env_map_rate));
  if (rate != g_applied_env_rate) {
    for (const RenderScenario& scenario : g_scenarios) SetEnvMapRate(scenario, rate);
    g_applied_env_rate = rate;
    REXLOG_INFO("Reflection updates x{:.2f} ({} render scenarios)", rate, g_scenarios.size());
  }
  if (!REXCVAR_GET(lavender_shadows) && !CarLightsOn()) {
    KeepShadowsOff();
  } else if (g_shadows_skipped) {
    for (const RenderScenario& scenario : g_scenarios) SetShadowsSkipped(scenario, false);
    g_shadows_skipped = false;
    REXLOG_INFO("Shadows on");
  }
}

size_t ScenarioCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_scenarios.size();
}

double SecondsOfDay() { return GameSecondsOfDay(); }

}  // namespace lavender_shift::render_settings
