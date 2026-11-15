// The trainer's credits (docs/PINYON_PARITY.md LP-2.12), Pinyon Shift's
// NP-8.3 with FH2's addresses (findings: Profile). As the title's own
// CGivePlayerMoney script action does: the profile is sub_8254F878 of the
// user at [[0x83422224] + 108], sub_82546378 reads its Main/Credits and
// sub_825521E8 sets it (clamped to 999,999,999), which also tells the
// balance's listeners (the HUD) and marks the field for the next save. Until
// the profile's values are loaded (the byte at +40) a change waits.

#include "trainer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include "diagnostics.h"
#include "guest_memory.h"

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

void Update() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next_read{};
  const bool pending = g_has_pending.load(std::memory_order_acquire);
  const auto now = Clock::now();
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
