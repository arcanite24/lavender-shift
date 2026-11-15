// The trainer's credits (docs/PINYON_PARITY.md LP-2.12), Pinyon Shift's
// NP-8.3 with FH2's addresses (findings: Profile). As the title's own
// CGivePlayerMoney script action does: the profile is sub_8254F878 of the
// user at [[0x83422224] + 108], sub_82546378 reads its Main/Credits and
// sub_825521E8 sets it (clamped to 999,999,999), which also tells the
// balance's listeners (the HUD) and marks the field for the next save. Until
// the profile's values are loaded (the byte at +40) a change waits.
//
// The time of day, Pinyon's NP-8.4 with FH2's identical layout: the world
// (as render_settings sees it, since FH1's holder path does not lead to it in
// FH2) has its clock at [world + 232] (sub_82496DA0), and the
// clock's float at +10488 is seconds since midnight, advanced and lit by
// sub_82688030 each frame. While a script holds the time (the byte at +10505)
// it is left alone, so events keep their own time.

#include "trainer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>

#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include "diagnostics.h"
#include "guest_memory.h"
#include "render_settings.h"

REXCVAR_DEFINE_DOUBLE(lavender_cheat_time_of_day, -1.0, "Cheats",
                      "Hold the time of day at this hour (0 to 24, fractions allowed); -1 lets "
                      "the title's clock run")
    .range(-1.0, 24.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(lavender_cheat_day_length_minutes, 0.0, "Cheats",
                      "How many real minutes 24 game hours last (1 to 1440); 0 keeps the "
                      "title's rate, -1 follows this PC's clock. Scripted times of day still "
                      "apply, and lavender_cheat_time_of_day wins over it")
    .range(-1.0, 1440.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::trainer {
namespace {

using namespace lavender_shift::guest;

constexpr uint32_t kUserHolder = 0x83422224u;
constexpr uint32_t kHolderUser = 108;
constexpr uint32_t kProfileLoaded = 40;
constexpr uint32_t kProfileFromUser = 0x8254F878u;
constexpr uint32_t kProfileCredits = 0x82546378u;
constexpr uint32_t kProfileSetCredits = 0x825521E8u;
constexpr int64_t kMaximumCredits = 999'999'999;
constexpr uint32_t kWorldTimeOfDay = 232;
constexpr uint32_t kTimeOfDaySeconds = 10488;
constexpr uint32_t kTimeOfDayScriptHold = 10505;
constexpr double kDay = 86400.0;

std::mutex g_mutex;
int64_t g_pending = 0;
std::atomic<bool> g_has_pending{false};
std::atomic<int64_t> g_credits{-1};

uint32_t Call(uint32_t address, uint32_t a0, uint32_t a1 = 0) {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->function_dispatcher()->GetFunction(address)) return 0;
  uint64_t args[6] = {a0, a1, 0, 0, 0, 0};
  return uint32_t(kernel_state->function_dispatcher()->ExecuteTrap(
      rex::runtime::ThreadState::Get(), address, args, 6));
}

// The signed-in player's profile once its values are loaded, else 0.
uint32_t Profile() {
  if (!Readable(kUserHolder, 4)) return 0;
  const uint32_t holder = LoadU32(kUserHolder);
  if (!holder || !Readable(holder + kHolderUser, 4)) return 0;
  const uint32_t user = LoadU32(holder + kHolderUser);
  if (!user) return 0;
  const uint32_t profile = Call(kProfileFromUser, user);
  if (!profile || !Readable(profile + kProfileLoaded, 1) || !LoadU8(profile + kProfileLoaded)) {
    return 0;
  }
  return profile;
}

// The world's time-of-day object, or 0 before free roam.
uint32_t Clock() {
  const uint32_t world = render_settings::World();
  if (!world || !Readable(world + kWorldTimeOfDay, 4)) return 0;
  const uint32_t clock = LoadU32(world + kWorldTimeOfDay);
  return clock && Readable(clock + kTimeOfDaySeconds, 20) ? clock : 0;
}

float LoadFloat(uint32_t address) {
  const uint32_t bits = LoadU32(address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void HoldTimeOfDay(double delta_seconds) {
  static double written = -1.0;
  static double last_logged = -1.0;
  const double hours = REXCVAR_GET(lavender_cheat_time_of_day);
  const double held = std::isfinite(hours) && hours >= 0.0 ? std::min(hours, 24.0) * 3600.0 : -1.0;
  const double day_minutes = REXCVAR_GET(lavender_cheat_day_length_minutes);
  if (held < 0.0 && day_minutes == 0.0) {
    written = -1.0;
    return;
  }
  const uint32_t clock = Clock();
  if (!clock || LoadU8(clock + kTimeOfDayScriptHold) != 0) {
    written = -1.0;
    return;
  }
  double seconds = held;
  if (held < 0.0) {
    if (day_minutes < 0.0) {
      const std::time_t now = std::time(nullptr);
      std::tm local{};
#if defined(_WIN32)
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      seconds = local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec;
    } else {
      const double current = LoadFloat(clock + kTimeOfDaySeconds);
      // Further than a quarter hour from what was written: the title set the
      // time itself.
      double drift = std::fabs(current - written);
      drift = std::min(drift, kDay - drift);
      if (written < 0.0 || !std::isfinite(current) || drift > 900.0) {
        written = std::isfinite(current) ? std::clamp(current, 0.0, kDay) : 0.0;
      }
      seconds = std::fmod(written + std::max(delta_seconds, 0.0) * kDay / (day_minutes * 60.0), kDay);
    }
  }
  // One event per game hour, so a route can time the day.
  if (last_logged < 0.0 || std::floor(seconds / 3600.0) != std::floor(last_logged / 3600.0)) {
    diagnostics::RecordEvent("cheat.day_clock", {{"seconds", fmt::format("{:.1f}", seconds)},
                                                 {"day_minutes", fmt::format("{:g}", day_minutes)}});
    last_logged = seconds;
  }
  written = seconds;
  const float value = float(seconds);
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreU32(clock + kTimeOfDaySeconds, bits);
}

}  // namespace

void AddCredits(int64_t amount) {
  std::lock_guard lock(g_mutex);
  g_pending += amount;
  g_has_pending.store(true, std::memory_order_release);
}

std::optional<uint32_t> Credits() {
  const int64_t credits = g_credits.load(std::memory_order_acquire);
  if (credits < 0) return std::nullopt;
  return uint32_t(credits);
}

void Update(double delta_seconds) {
  HoldTimeOfDay(delta_seconds);
  using Steady = std::chrono::steady_clock;
  static Steady::time_point next_read{};
  const bool pending = g_has_pending.load(std::memory_order_acquire);
  const auto now = Steady::now();
  if (!pending && now < next_read) return;
  next_read = now + std::chrono::seconds(1);
  const uint32_t profile = Profile();
  if (!profile) {
    g_credits.store(-1, std::memory_order_release);
    return;
  }
  const uint32_t previous = Call(kProfileCredits, profile);
  g_credits.store(previous, std::memory_order_release);
  if (!pending) return;
  int64_t add = 0;
  {
    std::lock_guard lock(g_mutex);
    add = g_pending;
    g_pending = 0;
    g_has_pending.store(false, std::memory_order_release);
  }
  if (add == 0) return;
  const int64_t target = std::clamp<int64_t>(int64_t(previous) + add, 0, kMaximumCredits);
  Call(kProfileSetCredits, profile, uint32_t(target));
  const uint32_t value = Call(kProfileCredits, profile);
  g_credits.store(value, std::memory_order_release);
  diagnostics::RecordEvent("cheat.credits", {{"added", std::to_string(add)},
                                             {"before", std::to_string(previous)},
                                             {"after", std::to_string(value)}});
}

}  // namespace lavender_shift::trainer
