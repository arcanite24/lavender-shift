// SAVE in the settings menu: the title's own "save the profile now", the
// call its event results, car purchases, garage screens and CSaveProfile
// state make (26 call sites), so the save is the game's, not a copy of its
// files.
//
// FH2 TU v2 addresses, from the generated code and the RTTI vtables:
// - 0x827ACEC8(this, user, show_ui): the full save. It writes the profile
//   file and the player database through CSecurityStream, then flushes the
//   save container (XamContentFlush). this = [0x8342220C], user -1 (the
//   signed-in one). show_ui 1 runs the save indicator, which yields until a
//   game timer moves; the title's async save tasks pass 0, and so does this.
// - 0x8248DC80(): the title's "may save now" test (free roam, race or menu
//   modes, nothing busy), checked first as the title's micro-saves do.
// - 0x8254F878(user): the CForzaProfile of the user at [[0x83422224]+108].
//   Its byte at +46 allows saving (the write function returns without it)
//   and +28 is its mounted save volume.
//
// The save runs synchronously at the start of the title's UI update
// (CUserInterface4, sub_83000358, on the simulation thread), where the
// title's own menu saves run. After writing, the save re-applies the
// profile's options (sub_825784B0 -> sub_8254A510), which post jobs to a
// queue whose current list (+52) is open only during parts of the frame:
// called from the simulation step's delta hook instead, it crashed there
// (sub_82404E18 reading the null list) after both files were written.

#include "manual_save.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include "guest_memory.h"

namespace {

constexpr uint32_t kSaveOwner = 0x8342220Cu;
constexpr uint32_t kGame = 0x83422224u;
constexpr uint32_t kSaveProfile = 0x827ACEC8u;
constexpr uint32_t kMaySave = 0x8248DC80u;
constexpr uint32_t kProfileOf = 0x8254F878u;

constexpr const char* kWaiting = "SAVES WHEN THE GAME ALLOWS";
// A request waits this long for the title to allow a save.
constexpr auto kPatience = std::chrono::minutes(5);

std::atomic<bool> requested{false};
// Simulation thread only.
bool pending = false;
std::chrono::steady_clock::time_point pending_since;
std::string last_reason;
std::mutex status_mutex;
std::string status;

void SetStatus(std::string text) {
  std::lock_guard lock(status_mutex);
  status = std::move(text);
}

uint32_t Pointer(uint32_t field, uint32_t size) {
  using namespace lavender_shift::guest;
  if (!Readable(field, 4)) return 0;
  const uint32_t value = LoadU32(field);
  return value && Readable(value, size) ? value : 0;
}

bool Callable(uint32_t address) {
  auto* kernel_state = rex::system::kernel_state();
  return kernel_state && kernel_state->function_dispatcher()->GetFunction(address);
}

uint32_t Call(uint32_t address, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0) {
  uint64_t args[6] = {a0, a1, a2, 0, 0, 0};
  return uint32_t(rex::system::kernel_state()->function_dispatcher()->ExecuteTrap(
      rex::runtime::ThreadState::Get(), address, args, 6));
}

std::string LocalTime() {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[16] = {};
  std::strftime(text, sizeof(text), "%H:%M", &local);
  return text;
}

// Why the title cannot save now, or empty when it can.
const char* Blocker(uint32_t& owner) {
  using namespace lavender_shift::guest;
  if (!Callable(kSaveProfile) || !Callable(kMaySave) || !Callable(kProfileOf)) {
    return "NOT AVAILABLE";
  }
  owner = Pointer(kSaveOwner, 8);
  const uint32_t game = Pointer(kGame, 112);
  const uint32_t user = game ? LoadU32(game + 108) : 0;
  if (!owner || !user) return "NO PROFILE YET";
  const uint32_t profile = Call(kProfileOf, user);
  if (!profile || !Readable(profile, 48)) return "NO PROFILE YET";
  if (!LoadU8(profile + 46) || !LoadU32(profile + 28)) return "SAVING IS OFF";
  if (!(Call(kMaySave) & 0xFF)) return kWaiting;
  return nullptr;
}

// Which of the title's may-save tests fails, for the log. They are, in
// sub_8248DC80's order, on the session s = [[0x83422224]+4]: a global test
// (0x82FDE678 on [0x83429C84]) is false; s exists; [0x82579398(s)] exists;
// 0x82FEB618(s) is true; byte s+60 is set; the mode of [0x82579398(s)]
// (0x82497A18) is 17, 18 or 19; byte s+48 is clear; 0x82764DC0() is false;
// and 0x82763FE0([0x834467C8]) is false when that object exists.
std::string MaySaveFailure() {
  using namespace lavender_shift::guest;
  const uint32_t game = Pointer(kGame, 8);
  const uint32_t session = game ? Pointer(game + 4, 64) : 0;
  const uint32_t global = Readable(0x83429C84u, 4) ? LoadU32(0x83429C84u) : 0;
  if (Call(0x82FDE678u, global) & 0xFF) return "global busy (0x82FDE678)";
  if (!session) return "no session";
  const uint32_t holder = Call(0x82579398u, session);
  const uint32_t state = holder && Readable(holder, 4) ? LoadU32(holder) : 0;
  if (!state) return "no state";
  if (!(Call(0x82FEB618u, session) & 0xFF)) return "0x82FEB618 false";
  if (!LoadU8(session + 60)) return "session+60 clear";
  const uint32_t mode = Call(0x82497A18u, state);
  if (mode < 17 || mode > 19) return "mode " + std::to_string(int32_t(mode));
  if (LoadU8(session + 48)) return "session+48 set";
  if (Call(0x82764DC0u) & 0xFF) return "0x82764DC0 true";
  const uint32_t other = Readable(0x834467C8u, 4) ? LoadU32(0x834467C8u) : 0;
  if (other && (Call(0x82763FE0u, other) & 0xFF)) return "0x82763FE0 true";
  return "none";
}

}  // namespace

namespace lavender_shift::manual_save {

void Request() {
  SetStatus("SAVING...");
  requested.store(true, std::memory_order_release);
}

bool Pending() { return pending || requested.load(std::memory_order_acquire); }

std::string Status() {
  std::lock_guard lock(status_mutex);
  return status;
}

void Update() {
  const auto now = std::chrono::steady_clock::now();
  if (requested.exchange(false, std::memory_order_acq_rel) && !pending) {
    pending = true;
    pending_since = now;
    last_reason.clear();
  }
  if (!pending) return;
  uint32_t owner = 0;
  if (const char* blocker = Blocker(owner)) {
    // The title refuses while its own menus are up (the settings menu can be
    // opened from its pause menu): try again each step until it allows it.
    if (blocker == kWaiting && now - pending_since < kPatience) {
      // Log each change of reason, with the time since the request, so the
      // log shows what held the save and when it let go.
      const std::string reason = MaySaveFailure();
      if (reason != last_reason) {
        const uint32_t busy = Pointer(0x83429C84u, 4);
        REXLOG_INFO("Manual save: waiting after {:.1f} s ({}; busy word {:08X})",
                    std::chrono::duration<double>(now - pending_since).count(), reason,
                    busy ? lavender_shift::guest::LoadU32(busy) : 0u);
        last_reason = reason;
      }
      // The title's busy flag is up while its own menus (the pause menu,
      // from which this menu is often opened) are: the save runs once the
      // player is back driving.
      SetStatus(reason.rfind("global busy", 0) == 0 ? "SAVES ONCE YOU RETURN TO THE GAME"
                                                    : blocker);
      return;
    }
    pending = false;
    if (blocker == kWaiting) {
      REXLOG_INFO("Manual save: not saved, the game did not allow it for {} min ({})",
                  kPatience.count(), MaySaveFailure());
      SetStatus("NOT SAVED, THE GAME IS BUSY");
    } else {
      REXLOG_INFO("Manual save: not saved ({})", blocker);
      SetStatus(blocker);
    }
    return;
  }
  pending = false;
  REXLOG_INFO("Manual save: allowed after {:.1f} s",
              std::chrono::duration<double>(now - pending_since).count());
  const auto start = std::chrono::steady_clock::now();
  Call(kSaveProfile, owner, 0xFFFFFFFFu, 0);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("Manual save: the title saved the profile in {:.0f} ms", ms);
  SetStatus("SAVED AT " + LocalTime());
}

}  // namespace lavender_shift::manual_save
