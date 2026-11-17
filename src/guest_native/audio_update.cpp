// Game audio logic at the console's rate, after Pinyon Shift's
// src/guest_native/audio_update.cpp (its NATIVE_REPLACEMENT_RESEARCH.md NR-1).
//
// The audio thread drains, once per rendered frame, a list of deferred calls
// the game thread posted. Each entry holds a marshalling thunk, the object
// and a copy of the arguments; the thunk makes the virtual call. In FH1 98 %
// of the thread's time is inside four of them, and FH2 has the same four
// (mapped from FH1 base; the thunks use the same vtable slots and argument
// layout, findings: Audio):
//
// - sub_82D404E8 (FH1 82BB9CD8, exact): vtable +88 with the frame time in f1
//   and a block of car state, the car audio model update (sub_82D4ED20);
// - sub_824F81E0 (FH1 8249BE98, byte-identical): vtable +44 with the frame
//   time and a flag, the audio engine update (sub_82516270);
// - sub_824F8310 (FH1 8249BFC8, exact): vtable +52 without a time
//   (sub_82503320);
// - sub_824F8488 (FH1 8249C130, exact): vtable +64 (sub_824FF5E0).
//
// FH1 drains the list once per rendered frame, so above 30 fps these run
// more often than on the console. FH2 drains it about 40-50 times a second
// whatever the frame rate (see Interval below).
// lavender_audio_update_interval N > 1 (2 by default) runs each of them on
// one call in up to N for each object and skips the others, as far as the
// measured drain rate leaves them at 30 Hz or more (so not at 40-50 Hz); the
// timed ones get the skipped calls' time added to their own, so the models'
// smoothing and timers keep their speed. The thunks are generic (any
// object's slot), so a call is skipped only when its object's vtable slot is
// the update named above; any other call through the same thunk always
// runs. The engine update also runs whenever its flag differs from the last
// run's. lavender_audio_update_mask picks the calls (bit 0 car models, 1
// engine update, 2 the +52 call, 3 the +64 call).

#include <array>
#include <chrono>
#include <string>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

#include "diagnostics.h"
#include "guest_native/guest_native.h"

REXCVAR_DEFINE_UINT32(lavender_audio_update_interval, 2, "Lavender Shift",
                      "Run the game's per-frame audio updates (car audio models, the audio "
                      "engine update and two others) on up to one frame in N, carrying the "
                      "skipped frames' time, and only as far as they stay at 30 Hz or more "
                      "(NR-1; 1 = every frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(lavender_audio_update_mask, 0xF, "Lavender Shift",
                      "Which audio updates lavender_audio_update_interval slows: bit 0 car "
                      "audio models, 1 audio engine update, 2 the +52 update, 3 the +64 update")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_audio_update_log, false, "Lavender Shift",
                    "Log the audio update gates' frame times, intervals and table use every "
                    "10 s")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::guest_native {
namespace {

uint8_t* Host(uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}
uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Host(base, address), sizeof(value));
  return std::byteswap(value);
}
float LoadF32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Load32(base, address));
}
void StoreF32(uint8_t* base, uint32_t address, float value) {
  const uint32_t bits = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(Host(base, address), &bits, sizeof(bits));
}

struct Command {
  uint32_t slot_offset;  // vtable offset the thunk calls
  uint32_t target;       // the update that slot holds
  bool timed;            // payload +4 holds the frame time
  bool flagged;          // payload +8 holds a flag byte
};
constexpr std::array<Command, 4> kCommands = {{
    {88, 0x82D4ED20u, true, false},
    {44, 0x82516270u, true, true},
    {52, 0x82503320u, false, false},
    {64, 0x824FF5E0u, false, false},
}};

// Only CAudioThread executes the lists.
struct Entry {
  uint32_t object = 0;
  uint32_t command = 0;
  uint32_t skipped = 0;
  float carried = 0.0f;
  uint8_t flag = 0;
};
constexpr size_t kMaxEntries = 64;
struct State {
  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  uint32_t carrying = 0;  // entries with time carried
  float frame_time = 0.0f;  // the last frame time a timed update was given
  double drain_seconds = 0.0;  // mean host time between drains
  std::chrono::steady_clock::time_point last_drain{};
};
State state;

// Calls to run one in, so that the updates keep at least the console's
// 30 Hz; never above the cvar. Unlike FH1's, FH2's audio thread does not
// drain once per rendered frame: on the desktop it drained about 40 times a
// second at 50 fps and 50 at 140 fps, and each update gets only the last
// frame's time. So the rate comes from the host time between drains (the
// engine update runs once in each), not from that frame time.
uint32_t Interval() {
  const uint32_t limit = REXCVAR_GET(lavender_audio_update_interval);
  if (limit <= 1 || state.drain_seconds <= 0.0) return 1;
  const uint32_t fit = uint32_t((1.0 / 30.0) / state.drain_seconds);
  return fit < 1 ? 1 : (fit < limit ? fit : limit);
}

// Called on each engine update: a running mean of the time between drains.
void NoteDrain() {
  const auto now = std::chrono::steady_clock::now();
  if (state.last_drain != std::chrono::steady_clock::time_point{}) {
    const double seconds = std::chrono::duration<double>(now - state.last_drain).count();
    if (seconds < 0.5) {
      state.drain_seconds =
          state.drain_seconds <= 0.0 ? seconds : state.drain_seconds * 0.9 + seconds * 0.1;
    }
  }
  state.last_drain = now;
}

Entry* Find(uint32_t command, uint32_t object, bool insert) {
  for (size_t i = 0; i < state.count; ++i) {
    Entry& e = state.entries[i];
    if (e.object == object && e.command == command) return &e;
  }
  if (!insert || state.count == kMaxEntries) return nullptr;
  Entry& e = state.entries[state.count++];
  e = Entry{};
  e.object = object;
  e.command = command;
  e.skipped = ~0u;  // the first call runs
  return &e;
}

void Run(Entry& e, const Command& c, PPCContext& ctx, uint8_t* base) {
  if (c.timed && e.carried != 0.0f) {
    const uint32_t time = ctx.r4.u32 + 4;
    StoreF32(base, time, LoadF32(base, time) + e.carried);
    --state.carrying;
  }
  e.carried = 0.0f;
  e.skipped = 0;
  if (c.flagged) e.flag = *Host(base, ctx.r4.u32 + 8);
}

// lavender_audio_update_log: per command, calls and how often the interval
// let a skip happen, the last frame time and the table's fill.
struct GateLog {
  std::array<uint64_t, 4> calls{}, interval_two{}, full{};
  std::chrono::steady_clock::time_point last{};
} gate_log;

void LogGate(size_t index, uint32_t interval) {
  ++gate_log.calls[index];
  if (interval > 1) ++gate_log.interval_two[index];
  if (state.count == kMaxEntries) ++gate_log.full[index];
  const auto now = std::chrono::steady_clock::now();
  if (gate_log.last == std::chrono::steady_clock::time_point{}) gate_log.last = now;
  if (now - gate_log.last < std::chrono::seconds(10)) return;
  gate_log.last = now;
  std::string calls, two, full;
  for (size_t i = 0; i < 4; ++i) {
    calls += std::to_string(gate_log.calls[i]) + (i < 3 ? "," : "");
    two += std::to_string(gate_log.interval_two[i]) + (i < 3 ? "," : "");
    full += std::to_string(gate_log.full[i]) + (i < 3 ? "," : "");
  }
  diagnostics::RecordEvent("audio.update_rate", {{"calls", calls},
                                                 {"interval_over_1", two},
                                                 {"table_full", full},
                                                 {"entries", std::to_string(state.count)},
                                                 {"frame_time", std::to_string(state.frame_time)},
                                                 {"drain_hz", std::to_string(state.drain_seconds > 0.0
                                                                                 ? 1.0 / state.drain_seconds
                                                                                 : 0.0)}});
  gate_log = GateLog{};
  gate_log.last = now;
}

template <size_t kIndex>
bool Gate(PPCContext& ctx, uint8_t* base) {
  const Command& c = kCommands[kIndex];
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  const uint32_t table = Load32(base, object);
  if (table == 0 || Load32(base, table + c.slot_offset) != c.target) return false;
  if (c.timed) {
    const float dt = LoadF32(base, ctx.r4.u32 + 4);
    if (dt > 0.0f && dt < 1.0f) state.frame_time = dt;
  }
  if (kIndex == 1) NoteDrain();
  const uint32_t interval = Interval();
  if (REXCVAR_GET(lavender_audio_update_log)) LogGate(kIndex, interval);
  const bool on = interval > 1 && (REXCVAR_GET(lavender_audio_update_mask) >> kIndex) & 1;
  if (!on && state.carrying == 0) return false;
  Entry* e = Find(kIndex, object, on);
  if (e == nullptr) return false;
  const bool flag_changed = c.flagged && *Host(base, ctx.r4.u32 + 8) != e->flag;
  if (on && !flag_changed && e->skipped + 1 < interval) {
    ++e->skipped;
    if (c.timed) {
      if (e->carried == 0.0f) ++state.carrying;
      e->carried += LoadF32(base, ctx.r4.u32 + 4);
    }
    return true;  // this frame's call waits for the next run
  }
  Run(*e, c, ctx, base);
  return false;  // the original runs, with the carried time
}

bool Enabled() { return true; }  // Gate decides per call (the interval is hot)

size_t TimedWritten(const PPCContext& ctx, uint8_t* /*base*/, GuestRange* ranges) {
  ranges[0] = {ctx.r4.u32 + 4, 4};
  return 1;
}

#define LAVENDER_AUDIO_REPLACEMENT(var, index, label, address, size, hash, written_fn)   Replacement var = {                                                                    .name = label,                                                                     .guest_address = address,                                                          .guest_code_size = size,                                                           .guest_code_fnv1a = hash,                                                          .returns = kReturnNone,                                                            .enabled = Enabled,                                                                .native = Gate<index>,                                                             .written = written_fn,                                                         };

LAVENDER_AUDIO_REPLACEMENT(car_models_replacement, 0, "audio.update_rate.car_models",
                           0x82D404E8u, 96, 0x75A3AB1F079A3D79ull, TimedWritten)
LAVENDER_AUDIO_REPLACEMENT(engine_replacement, 1, "audio.update_rate.engine", 0x824F81E0u, 24,
                           0x11094E4E9A208668ull, TimedWritten)
LAVENDER_AUDIO_REPLACEMENT(slot52_replacement, 2, "audio.update_rate.slot52", 0x824F8310u, 96,
                           0x6AB853CA669F12ADull, nullptr)
LAVENDER_AUDIO_REPLACEMENT(slot64_replacement, 3, "audio.update_rate.slot64", 0x824F8488u, 96,
                           0x9DD17BD4445C514Bull, nullptr)

#undef LAVENDER_AUDIO_REPLACEMENT

}  // namespace
}  // namespace lavender_shift::guest_native

LAVENDER_GUEST_NATIVE(82D404E8, ::lavender_shift::guest_native::car_models_replacement)
LAVENDER_GUEST_NATIVE(824F81E0, ::lavender_shift::guest_native::engine_replacement)
LAVENDER_GUEST_NATIVE(824F8310, ::lavender_shift::guest_native::slot52_replacement)
LAVENDER_GUEST_NATIVE(824F8488, ::lavender_shift::guest_native::slot64_replacement)
