// FH2 code that advances state by a constant step per update, written for
// the console's 30 updates a second (docs/PINYON_PARITY.md LP-3.3). Lavender
// renders 60 frames a second by default, so these ran at twice their speed.
// Pinyon Shift's fixes, at the same code in FH2 (each site's instruction
// word matches FH1 v4's; functions aligned with the mapper's normalization):
//
// - The scripted UI camera shots (CUI4AnimatedCamera::Update, TU v2
//   sub_8279A300, FH1 v4 sub_826F2900): its step in f1 becomes the real time
//   since the same camera's previous update times 30.
// - The trackside crowd's animation frames (TU v2 sub_82F95758, FH1 v4
//   sub_82ECAB38, on the render thread): each call gets the whole 30 Hz
//   steps of real time since the same models' previous call, the fraction
//   carried to the next.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"

REXCVAR_DEFINE_BOOL(lavender_fixed_steps_real_time, true, "Lavender Shift",
                    "Scale FH2's constant per-update steps, written for 30 updates a second, by "
                    "the real time between updates, so the UI camera shots and the crowds keep "
                    "their speed at 60 frames a second")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_fixed_steps_log, false, "Lavender Shift",
                    "Log every two seconds how often each fixed-step site runs and how much "
                    "it advances a second, before and after the scaling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using Clock = std::chrono::steady_clock;

// Per-site counts for lavender_fixed_steps_log.
struct RateLog {
  const char* name;
  Clock::time_point window{};
  uint32_t calls = 0;
  double fixed = 0.0, applied = 0.0;

  void Add(Clock::time_point now, double fixed_step, double applied_step) {
    if (!REXCVAR_GET(lavender_fixed_steps_log)) return;
    ++calls;
    fixed += fixed_step;
    applied += applied_step;
    if (now - window < std::chrono::seconds(2)) return;
    if (window != Clock::time_point{} && calls) {
      const double seconds = std::chrono::duration<double>(now - window).count();
      lavender_shift::diagnostics::RecordEvent(
          "fixed_step.rate", {{"site", name},
                              {"updates_per_second", fmt::format("{:.1f}", calls / seconds)},
                              {"fixed_per_second", fmt::format("{:.3f}", fixed / seconds)},
                              {"applied_per_second", fmt::format("{:.3f}", applied / seconds)}});
    }
    window = now;
    calls = 0;
    fixed = applied = 0.0;
  }
};

// The UI camera: a gap over 0.25 s (a new shot, a pause) keeps the constant
// for that call.
struct CameraClocks {
  struct Entry {
    uint32_t object = 0;
    Clock::time_point last;
  };
  std::mutex mutex;
  std::array<Entry, 8> entries{};
  RateLog log{"ui_camera"};
} g_camera;

struct CrowdClock {
  Clock::time_point last;
  double carry = 0.0;
};
std::mutex g_crowd_mutex;
std::unordered_map<uint32_t, CrowdClock> g_crowd_clocks;
RateLog g_crowd_log{"crowd"};
thread_local uint32_t t_crowd_steps = 1;

void ApplyCrowdSteps(PPCRegister& r10) {
  if (REXCVAR_GET(lavender_fixed_steps_real_time) && r10.u32 != 0) {
    r10.u64 = t_crowd_steps;
  }
}

}  // namespace

// At TU v2 0x8279A368 (FH1 v4 0x826F2968): r31 the camera, f1 its step (1/30
// s, or 1/60 under a UI flag), about to advance the shot by it.
void LavenderScaleAnimatedCameraStep(PPCRegister& r31, PPCRegister& f1) {
  const Clock::time_point now = Clock::now();
  const uint32_t object = r31.u32;
  std::lock_guard lock(g_camera.mutex);
  CameraClocks::Entry* clock = nullptr;
  for (auto& entry : g_camera.entries) {
    if (entry.object == object && entry.last != Clock::time_point{}) clock = &entry;
  }
  if (!clock) {
    clock = &*std::min_element(g_camera.entries.begin(), g_camera.entries.end(),
                               [](const auto& a, const auto& b) { return a.last < b.last; });
    *clock = {object, {}};
  }
  const double fixed = f1.f64;
  double applied = fixed;
  if (clock->last != Clock::time_point{} && REXCVAR_GET(lavender_fixed_steps_real_time)) {
    const double elapsed = std::chrono::duration<double>(now - clock->last).count();
    if (elapsed >= 0.0 && elapsed < 0.25) {
      applied = fixed * std::min(elapsed, 0.1) * 30.0;
      f1.f64 = applied;
    }
  }
  clock->last = now;
  g_camera.log.Add(now, fixed, applied);
}

// At TU v2 0x82F95788 (FH1 v4 0x82ECAB68), after the crowd update's prologue:
// r29 is the procedural models object.
void LavenderCrowdStepBegin(PPCRegister& r29) {
  const Clock::time_point now = Clock::now();
  std::lock_guard lock(g_crowd_mutex);
  if (g_crowd_clocks.size() >= 1024) g_crowd_clocks.clear();
  CrowdClock& clock = g_crowd_clocks[r29.u32];
  uint32_t steps = 1;
  if (clock.last != Clock::time_point{}) {
    const double elapsed = std::chrono::duration<double>(now - clock.last).count();
    if (elapsed >= 0.0 && elapsed < 0.25) {
      clock.carry += elapsed * 30.0;
      steps = uint32_t(clock.carry);
      clock.carry -= steps;
    } else {
      clock.carry = 0.0;
    }
  }
  clock.last = now;
  t_crowd_steps = steps;
  const bool on = REXCVAR_GET(lavender_fixed_steps_real_time);
  g_crowd_log.Add(now, 1.0, on ? double(steps) : 1.0);
}

// The title's step (r10) about to be added to a crowd instance's frame: at
// TU v2 0x82F95EC4 the instances that pick a random clip at the end of one
// (FH1 v4 0x82ECB27C), at 0x82F961F0 the looping ones (FH1 v4 0x82ECB594).
void LavenderCrowdStepRandom(PPCRegister& r10) { ApplyCrowdSteps(r10); }
void LavenderCrowdStepLooping(PPCRegister& r10) { ApplyCrowdSteps(r10); }
