// Cheats on the player's own profile, on by default (docs/PINYON_PARITY.md
// LP-2.14). Pinyon Shift runs its trainer on a separate cheats profile;
// Lavender does not: each cheat changes what the game reads while it runs,
// never the database or the save's own records, and turning one off takes
// effect at the next event (or the next AI spawn).
//
// - lavender_cheat_any_car_events: any car can enter any event. FH2 reads an
//   event's EventRestrictions rows in TU v2 sub_82759878 (FH1 v4
//   sub_826AEFF8, exact): the hook skips the decode of a row's Value into
//   the record the function has just set up with no limits, so the record is
//   still added. The event's class is stamped into every bucket-0 record by
//   sub_82743170 (FH1 v4 sub_8269B3B8, exact): the hook skips from after its
//   frame is set up to its epilogue.
// - lavender_cheat_no_catch_up: AI racers behind the player get no help to
//   catch up. Just after FH2 fills a spawning AI racer's config (TU v2
//   0x82511B18 in sub_82511790, FH1 v4 0x82521228 in sub_82520EA0, aligned;
//   the filler sub_82B2D6C0 and the AISkills and AIRubberbands readers it
//   calls are FH1 v4's after masking), the catch-up distances move out of
//   reach with a neutral factor, and the maximum skill and torque boost an
//   AI gets when it trails turn off. The stock rubber band stays: an AI far
//   ahead still eases off (torque cut or a mistake). Removing that half too
//   (Pinyon's "fair") left leaders out of reach and trailing AI far behind
//   once passed, which played worse. Skill and difficulty stay as the event
//   sets them.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"
#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(lavender_cheat_any_car_events, true, "Cheats",
                    "Any car can enter any event: the event's car and class restrictions "
                    "are not applied")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_cheat_no_catch_up, true, "Cheats",
                    "AI racers behind the player get no help to catch up; leaders still ease "
                    "off as in the stock game (applies to the AI racers spawned from then on)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace guest = lavender_shift::guest;

void LogOnce(std::atomic<bool>& logged, const char* name,
             std::initializer_list<lavender_shift::diagnostics::Field> fields = {}) {
  if (logged.exchange(true, std::memory_order_acq_rel)) return;
  std::vector<lavender_shift::diagnostics::Field> all = {{"name", name}};
  all.insert(all.end(), fields.begin(), fields.end());
  lavender_shift::diagnostics::RecordEvent("cheat.applied", all);
}

}  // namespace

// TU v2 0x8275998C (FH1 v4 0x826AF10C), jumping to 0x82759998: r25 is the
// event id whose restriction row is about to be decoded.
bool LavenderAnyCarEventRestriction(PPCRegister& r25) {
  if (!REXCVAR_GET(lavender_cheat_any_car_events)) return false;
  static std::mutex mutex;
  static std::set<uint32_t> logged;
  bool first = false;
  {
    std::lock_guard lock(mutex);
    first = logged.size() < 64 && logged.insert(r25.u32).second;
  }
  if (first) {
    lavender_shift::diagnostics::RecordEvent(
        "cheat.applied",
        {{"name", "lavender_cheat_any_car_events"}, {"event_id", std::to_string(r25.u32)}});
  }
  return true;
}

// TU v2 0x8274317C (FH1 v4 0x8269B3C4), jumping to the epilogue at
// 0x82743240: r4 is the limit derived from the event's TargetClass.
bool LavenderAnyCarEventClass(PPCRegister& r4) {
  if (!REXCVAR_GET(lavender_cheat_any_car_events)) return false;
  static std::atomic<bool> logged{false};
  LogOnce(logged, "lavender_cheat_any_car_events",
          {{"class_limit", std::to_string(r4.u32 & 0xFFu)}});
  return true;
}

// TU v2 0x82511B18 (FH1 v4 0x82521228): the AI racer's config at r1+464 has
// just been filled. Offsets from the AISkills and AIRubberbands readers.
void LavenderAiSpawnConfig(PPCRegister& r1) {
  if (!REXCVAR_GET(lavender_cheat_no_catch_up)) return;
  const uint32_t config = r1.u32 + 464;
  if (!guest::Readable(config, 452)) return;
  constexpr float kOutOfReach = 100000.0f;
  // Trailing help. The rubber band at +416 to +432 (an AI ahead easing off)
  // is left as the event set it.
  guest::StoreF32(config + 336, 0.0f);         // DistBehindForMaxPerformance: off
  guest::StoreF32(config + 344, 0.0f);         // DistBehindForTorqueBoost: off
  guest::StoreF32(config + 348, 1.0f);         // FarBehindTorqueBoost: none
  guest::StoreU32(config + 360, 0);            // TorqueBoostCheatLevel
  guest::StoreF32(config + 436, kOutOfReach);  // StartCatchUpDistance
  guest::StoreF32(config + 440, kOutOfReach);  // StopCatchUpDistance
  guest::StoreF32(config + 444, 0.0f);         // CatchUpFactor
  static std::atomic<uint32_t> spawned{0};
  const uint32_t count = spawned.fetch_add(1, std::memory_order_relaxed);
  if (count < 32) {
    lavender_shift::diagnostics::RecordEvent(
        "cheat.applied",
        {{"name", "lavender_cheat_no_catch_up"}, {"config", fmt::format("{:08X}", config)}});
  }
}
