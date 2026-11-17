// The loading screen's minimum time on screen, after Pinyon Shift's
// src/guest_native/loading_screen.cpp (its docs/CONSOLE_ERA_RESEARCH.md,
// "Loading screen minimum time").
//
// When the title shows its tip loading screen, the renderer's show method
// (sub_8261C510; FH1 base sub_825A2CB8, an exact match) sets a deadline at
// renderer +2088: the render stopwatch plus a minimum time on screen (float
// +2072). The main thread's load step yields until the render frame clears
// that deadline (-1.0), so a load that finishes early still waits it out. On
// a disc the load outlasted it; from a modern drive it is pure waiting.
//
// With lavender_loading_screen_minimum off (the default), this wrapper runs
// the title's show method and then clears the deadline, as the render frame
// would once it passed. The loading screen still appears and fades; it
// leaves when loading is done.

#include <bit>
#include <cstdint>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "diagnostics.h"
#include "guest_native/guest_native.h"

REXCVAR_DEFINE_BOOL(lavender_loading_screen_minimum, false, "Lavender Shift",
                    "Keep the console's minimum time on screen for loading screens even when "
                    "loading finished sooner")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" REX_FUNC(__imp__sub_8261C510);

namespace lavender_shift::guest_native {
namespace {

constexpr uint32_t kMinimumOffset = 2072;
constexpr uint32_t kDeadlineOffset = 2088;
// -1.0 as a double: the render frame's "no deadline" value.
constexpr uint64_t kNoDeadline = 0xBFF0000000000000ull;

bool SkipMinimum() { return !REXCVAR_GET(lavender_loading_screen_minimum); }

bool ShowWithoutMinimum(PPCContext& ctx, uint8_t* base) {
  const uint32_t renderer = ctx.r3.u32;
  __imp__sub_8261C510(ctx, base);
  auto* deadline = rex::memory::GuestPtr<rex::be_u64*>(base, renderer + kDeadlineOffset);
  if (uint64_t(*deadline) != kNoDeadline) {
    const float minimum = std::bit_cast<float>(
        uint32_t(*rex::memory::GuestPtr<rex::be_u32*>(base, renderer + kMinimumOffset)));
    diagnostics::RecordEvent("loading_screen.minimum_skipped",
                             {{"seconds", fmt::format("{:.2f}", minimum)}});
  }
  *deadline = kNoDeadline;
  return true;
}

Replacement show_replacement = {
    .name = "render.loading_screen_minimum",
    .guest_address = 0x8261C510u,
    .guest_code_size = 132,
    .guest_code_fnv1a = 0x94732EF18E2D7012ull,
    .returns = kReturnNone,
    .enabled = SkipMinimum,
    .native = ShowWithoutMinimum,
    .written = nullptr,
};

}  // namespace
}  // namespace lavender_shift::guest_native

LAVENDER_GUEST_NATIVE(8261C510, ::lavender_shift::guest_native::show_replacement)
