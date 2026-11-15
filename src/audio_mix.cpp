// The sound mix (docs/PINYON_PARITY.md LP-2.8). FH2 reads each sound
// category's level per mix state from media\audio\AudioMixerSnapshots.xml
// at start, as FH1 does; Pinyon Shift rewrites that file for the next
// start. Lavender applies settings live, so it edits the parsed table in
// guest memory instead.
//
// The table: the file's 44 snapshots in file order, 0x214 bytes apart,
// each 22 category records of 24 bytes {u8 active, f32 destvolume,
// f32 returntime, f32 returnfadetime, f32 fadetime, u32 category} in the
// file's category order. Addresses here point at the first destvolume
// (docs/findings/systems.toml, audio.mixer_table).

#include "audio_mix.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "ai_driver.h"
#include "guest_memory.h"

REXCVAR_DEFINE_INT32(lavender_mix_engine, 100, "Lavender Shift",
                     "Your car's engine level in percent (0-200)")
    .range(0, 200);
REXCVAR_DEFINE_INT32(lavender_mix_opponents, 100, "Lavender Shift",
                     "Other cars' level in percent (0-200)")
    .range(0, 200);
REXCVAR_DEFINE_INT32(lavender_mix_tyres, 100, "Lavender Shift",
                     "Tyre level in percent (0-200)")
    .range(0, 200);
REXCVAR_DEFINE_INT32(lavender_mix_wind, 100, "Lavender Shift",
                     "Wind level in percent (0-200)")
    .range(0, 200);
REXCVAR_DEFINE_INT32(lavender_mix_ambience, 100, "Lavender Shift",
                     "Ambience level in percent (0-200)")
    .range(0, 200);
REXCVAR_DEFINE_INT32(lavender_mix_music, 100, "Lavender Shift",
                     "Radio and music level in percent (0-200)")
    .range(0, 200);

namespace {

using namespace lavender_shift::guest;

constexpr uint32_t kSnapshots = 44;
constexpr uint32_t kCategories = 22;
constexpr uint32_t kSnapshotStride = 0x214;
constexpr uint32_t kRecordStride = 24;

// Snapshots in the Gameplay group, the ones Pinyon scales: scaling the
// menu and cutscene groups too left FH1's engine near silent.
constexpr std::array<uint32_t, 20> kGameplay = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                                10, 11, 12, 13, 14, 15, 33, 34, 42, 43};

// FreeRoam's levels, the table's first snapshot.
constexpr std::array<float, kCategories> kFreeRoam = {
    0.35f, 0.35f, 1.0f, 0.5f, 0.3f, 0.3f, 0.5f, 1.0f, 0.3f, 0.3f, 1.0f,
    1.0f,  0.5f,  1.0f, 0.8f, 0.8f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

struct Setting {
  int32_t (*percent)();
  std::array<int8_t, 3> categories;  // -1 ends the list
};

const std::array<Setting, 6> kSettings = {{
    {[] { return REXCVAR_GET(lavender_mix_engine); }, {0, -1, -1}},          // FocusCar
    {[] { return REXCVAR_GET(lavender_mix_opponents); }, {1, -1, -1}},       // OppCar
    {[] { return REXCVAR_GET(lavender_mix_tyres); }, {9, -1, -1}},           // Tire
    {[] { return REXCVAR_GET(lavender_mix_wind); }, {8, -1, -1}},            // CarWind
    {[] { return REXCVAR_GET(lavender_mix_ambience); }, {11, 12, 14}},       // Ambient*
    {[] { return REXCVAR_GET(lavender_mix_music); }, {13, 15, -1}},          // GameMusic, HomespaceMusic
}};

uint32_t table = 0;
bool searched = false;
std::array<float, kSnapshots * kCategories> original{};
std::array<int32_t, kSettings.size()> applied{};

// The mixer object holds the snapshots at +1544, so the first dest at +1548.
// 0x82D34400(mixer, snapshot) is the game's snapshot switch: it fades each
// category to the snapshot's levels and records it as its group's current
// one (+24968 + 4 * group); the groups' levels multiply. It does not skip a
// snapshot already playing, so calling it with the Gameplay group's current
// one re-applies an edit with the game's own fades; the levels otherwise
// reach FMOD only on a switch. It also records the last snapshot set and the
// Gameplay one before it, which are put back.
constexpr uint32_t kTableInMixer = 1548;
constexpr uint32_t kGroupInSnapshot = 524;
constexpr uint32_t kGameplayGroup = 2;
constexpr uint32_t kCurrentByGroup = 24968;
constexpr std::array<uint32_t, 2> kSwitchBookkeeping = {24952, 24964};
constexpr uint32_t kSwitchSnapshot = 0x82D34400u;

uint32_t Call(uint32_t address, uint32_t a0, uint32_t a1) {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->function_dispatcher()->GetFunction(address)) return 0;
  uint64_t args[6] = {a0, a1, 0, 0, 0, 0};
  return uint32_t(kernel_state->function_dispatcher()->ExecuteTrap(
      rex::runtime::ThreadState::Get(), address, args, 6));
}

uint32_t Record(uint32_t snapshot, uint32_t category) {
  return table + snapshot * kSnapshotStride + category * kRecordStride;
}

bool IsFreeRoam(uint32_t at, uint64_t end) {
  if (at + kSnapshots * kSnapshotStride > end) return false;
  for (uint32_t k = 0; k < kCategories; ++k) {
    const uint32_t record = at + k * kRecordStride;
    if (LoadF32(record) != kFreeRoam[k] || LoadU32(record + 16) != k) return false;
  }
  return true;
}

// Committed regions only: a reserved page passes the range check but faults.
uint32_t Find() {
  auto* memory = rex::system::kernel_state()->memory();
  for (uint64_t base = 0x10000; base < 0x70000000ull;) {
    auto* heap = memory->LookupHeap(uint32_t(base));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(base), &info) || !info.region_size) {
      base += 0x10000;
      continue;
    }
    const uint64_t end = uint64_t(info.base_address) + info.region_size;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) || end <= base) {
      base = end > base ? end : base + 0x1000;
      continue;
    }
    for (uint32_t at = uint32_t(base); at + 4 <= end; at += 4) {
      if (LoadF32(at) == kFreeRoam[0] && IsFreeRoam(at, end)) return at;
    }
    base = end;
  }
  return 0;
}

}  // namespace

namespace lavender_shift::audio_mix {

void Update() {
  if (!table) {
    // The table is parsed at start; look once the world has loaded.
    if (searched || ai_driver::GameMode() != 17) return;
    searched = true;
    table = Find();
    REXLOG_INFO("Audio mix: mixer table at {:08X}", table);
    // The layout check: FreeRoam belongs to the Gameplay group.
    if (table && LoadU32(table + kGroupInSnapshot) != kGameplayGroup) {
      REXLOG_WARN("Audio mix: unexpected group {} at {:08X}", LoadU32(table + kGroupInSnapshot),
                  table);
      table = 0;
    }
    if (!table) return;
    for (uint32_t s = 0; s < kSnapshots; ++s) {
      for (uint32_t c = 0; c < kCategories; ++c) {
        original[s * kCategories + c] = LoadF32(Record(s, c));
      }
    }
    applied.fill(100);
  }
  bool changed = false;
  for (size_t n = 0; n < kSettings.size(); ++n) {
    changed |= std::clamp(kSettings[n].percent(), 0, 200) != applied[n];
  }
  if (!changed) return;
  for (size_t n = 0; n < kSettings.size(); ++n) {
    const int32_t percent = std::clamp(kSettings[n].percent(), 0, 200);
    if (percent == applied[n]) continue;
    applied[n] = percent;
    for (const int8_t category : kSettings[n].categories) {
      if (category < 0) break;
      for (const uint32_t snapshot : kGameplay) {
        StoreF32(Record(snapshot, category),
                 original[snapshot * kCategories + category] * float(percent) / 100.0f);
      }
    }
    REXLOG_INFO("Audio mix: setting {} at {}%", n, percent);
  }
  // Re-apply the Gameplay group's snapshot.
  const uint32_t mixer = table - kTableInMixer;
  const uint32_t current = LoadU32(mixer + kCurrentByGroup + 4 * kGameplayGroup);
  if (std::find(kGameplay.begin(), kGameplay.end(), current) == kGameplay.end()) return;
  std::array<uint32_t, kSwitchBookkeeping.size()> kept{};
  for (size_t n = 0; n < kept.size(); ++n) kept[n] = LoadU32(mixer + kSwitchBookkeeping[n]);
  Call(kSwitchSnapshot, mixer, current);
  for (size_t n = 0; n < kept.size(); ++n) StoreU32(mixer + kSwitchBookkeeping[n], kept[n]);
  REXLOG_INFO("Audio mix: re-applied snapshot {}", current);
}

}  // namespace lavender_shift::audio_mix
