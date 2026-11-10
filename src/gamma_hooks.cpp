// FH2's gamma ramp, rebuilt every frame (docs/PINYON_PARITY.md LP-4.3).
// Pinyon Shift's hooks (its src/native_renderer/graphics_hooks.cpp) at the
// same instructions in FH2: the builder, TU v2 sub_82DAF4F8, is FH1 v4's
// sub_82CE8948 (mapper: aligned) with only two float constants loaded from
// moved addresses, and the device call it makes, D3D's SetPWLGamma (TU v2
// sub_82B748F8, FH1 v4 sub_82AAEE68), is identical after masking, including
// the device's copy of the converted ramp at +15408.
//
// The builder (r3 the display object, f1 the exponent, f2 and f3 the offset
// and range, r4 and r5 the curve and encoding) fills a ramp on its stack, as
// 128 piecewise-linear entries a channel (r1 + 128) or 256 entries a channel
// (r1 + 1664), 1,536 bytes either way, and hands it to the device. The hooks
// at the start of each format's path find a ramp built from the same
// arguments, put it back on the stack and jump to the device call; the hooks
// at the device call keep each newly built ramp, and skip the call when the
// ramp and the device's copy are what its last call left (the call would
// then find them equal and do nothing).

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"

REXCVAR_DEFINE_BOOL(lavender_reuse_gamma_ramp, true, "Lavender Shift",
                    "Reuse FH2's gamma ramp when it is rebuilt from the same settings (it is "
                    "rebuilt every frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_skip_unchanged_device_gamma, true, "Lavender Shift",
                    "Skip D3D's SetPWLGamma when the ramp and the device's copy are what its "
                    "last call left")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_gamma_log, false, "Lavender Shift",
                    "Log every ten seconds how often the gamma ramp was built, reused and "
                    "sent to the device")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}

struct GammaRamp {
  bool valid = false;
  uint64_t key[4] = {};
  uint8_t data[1536];
};
constexpr uint32_t kGammaRampOffset[2] = {128, 1664};
thread_local GammaRamp gamma_ramps[2];
thread_local int gamma_ramp_pending = -1;
thread_local uint64_t gamma_ramp_pending_key[4];

constexpr uint32_t kDeviceGammaCopy = 15408;
struct DeviceGamma {
  bool valid = false;
  uint32_t device = 0;
  uint8_t ramp[1536];
  uint8_t copy[1536];
};
thread_local DeviceGamma device_gamma;
thread_local uint8_t device_gamma_pending_ramp[1536];

// Counts for lavender_gamma_log; the builder runs on the render thread only.
struct Counts {
  uint64_t builds = 0, reused = 0, device_calls = 0, device_skipped = 0;
  uint64_t build_ns = 0, device_ns = 0;  // time in the builds and device calls that ran
  std::chrono::steady_clock::time_point window{};
} counts;

void Count(uint64_t Counts::*field) {
  if (!REXCVAR_GET(lavender_gamma_log)) return;
  ++(counts.*field);
  const auto now = std::chrono::steady_clock::now();
  if (counts.window == std::chrono::steady_clock::time_point{}) counts.window = now;
  if (now - counts.window < std::chrono::seconds(10)) return;
  lavender_shift::diagnostics::RecordEvent(
      "gamma.rate", {{"built", std::to_string(counts.builds)},
                     {"reused", std::to_string(counts.reused)},
                     {"device_calls", std::to_string(counts.device_calls)},
                     {"device_skipped", std::to_string(counts.device_skipped)},
                     {"build_ns", std::to_string(counts.build_ns)},
                     {"device_ns", std::to_string(counts.device_ns)},
                     {"seconds", "10"}});
  counts = {};
  counts.window = now;
}

using Clock = std::chrono::steady_clock;
thread_local Clock::time_point t_mark;
thread_local bool t_timing = false;

void Mark() {
  if (REXCVAR_GET(lavender_gamma_log)) {
    t_mark = Clock::now();
    t_timing = true;
  }
}

void AddSinceMark(uint64_t Counts::*field) {
  if (!t_timing) return;
  t_timing = false;
  counts.*field += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - t_mark)
                                .count());
}

// At the start of a path: f28 holds f1, f27 f3 + 1, f2 is still the argument,
// r26 and r25 hold r4 and r5, r24 holds r3.
bool GammaRampBegin(PPCContext& ctx, uint8_t* base, int path) {
  gamma_ramp_pending = -1;
  Count(&Counts::builds);
  Mark();
  if (!REXCVAR_GET(lavender_reuse_gamma_ramp)) return false;
  const uint64_t key[4] = {ctx.f28.u64, ctx.f27.u64, ctx.f2.u64,
                           (uint64_t(ctx.r26.u32) << 32) | ctx.r25.u32};
  GammaRamp& ramp = gamma_ramps[path];
  if (!ramp.valid || std::memcmp(ramp.key, key, sizeof(key))) {
    std::memcpy(gamma_ramp_pending_key, key, sizeof(key));
    gamma_ramp_pending = path;
    return false;
  }
  Count(&Counts::reused);
  t_timing = false;
  const uint32_t stack = ctx.r1.u32 + kGammaRampOffset[path];
  std::memcpy(base + stack, ramp.data, sizeof(ramp.data));
  // The arguments of the device call the hook jumps to.
  ctx.r3.u64 = Load32(base, ctx.r24.u32 + 20);
  ctx.r4.u64 = 2;
  ctx.r5.u64 = stack;
  return true;
}

void GammaRampBuilt(PPCContext& ctx, uint8_t* base, int path) {
  if (gamma_ramp_pending != path) return;
  gamma_ramp_pending = -1;
  GammaRamp& ramp = gamma_ramps[path];
  std::memcpy(ramp.data, base + ctx.r1.u32 + kGammaRampOffset[path], sizeof(ramp.data));
  std::memcpy(ramp.key, gamma_ramp_pending_key, sizeof(ramp.key));
  ramp.valid = true;
}

}  // namespace

// TU v2 0x82DAF540 (FH1 v4 0x82CE8990): the piecewise-linear path begins.
bool LavenderGammaRampPwlBegin(PPCContext& ctx, uint8_t* base) {
  return GammaRampBegin(ctx, base, 0);
}

// TU v2 0x82DAF718 (FH1 v4 0x82CE8B68), the SetPWLGamma call: keeps a newly
// built ramp, then skips the call when it would change nothing.
bool LavenderGammaRampPwlBuilt(PPCContext& ctx, uint8_t* base) {
  AddSinceMark(&Counts::build_ns);
  GammaRampBuilt(ctx, base, 0);
  const uint8_t* ramp = base + ctx.r5.u32;
  std::memcpy(device_gamma_pending_ramp, ramp, sizeof(device_gamma_pending_ramp));
  const bool skip = REXCVAR_GET(lavender_skip_unchanged_device_gamma) && device_gamma.valid &&
                    device_gamma.device == ctx.r3.u32 &&
                    !std::memcmp(device_gamma.ramp, ramp, sizeof(device_gamma.ramp)) &&
                    !std::memcmp(device_gamma.copy, base + ctx.r3.u32 + kDeviceGammaCopy,
                                 sizeof(device_gamma.copy));
  Count(skip ? &Counts::device_skipped : &Counts::device_calls);
  if (!skip) Mark();
  return skip;
}

// TU v2 0x82DAF71C, after the device call or its skip: the device's copy
// now matches the ramp just passed. r24 still holds the builder's r3, whose
// +20 is the device.
void LavenderGammaRampPwlDeviceSet(PPCContext& ctx, uint8_t* base) {
  AddSinceMark(&Counts::device_ns);
  const uint32_t device = Load32(base, ctx.r24.u32 + 20);
  device_gamma.device = device;
  std::memcpy(device_gamma.ramp, device_gamma_pending_ramp, sizeof(device_gamma.ramp));
  std::memcpy(device_gamma.copy, base + device + kDeviceGammaCopy, sizeof(device_gamma.copy));
  device_gamma.valid = true;
}

// TU v2 0x82DAF720 and 0x82DAF848: the 256-entry path's start and its
// device call.
bool LavenderGammaRampLinearBegin(PPCContext& ctx, uint8_t* base) {
  return GammaRampBegin(ctx, base, 1);
}

void LavenderGammaRampLinearBuilt(PPCContext& ctx, uint8_t* base) {
  AddSinceMark(&Counts::build_ns);
  GammaRampBuilt(ctx, base, 1);
}
