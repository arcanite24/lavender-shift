// User interface update at a lower rate, after Pinyon Shift's
// src/guest_native/ui_update.cpp (its #1071 P3).
//
// The simulation thread updates the user interface once a frame through
// CUserInterface4's update (sub_83000358; FH1 base sub_82E71F90, mapped by
// context and the same 150 instructions after masking addresses and
// offsets). It takes the frame time in f1 and walks the scene manager, the
// custom controls (the minimap renderer and its layers, the HUD stackers)
// and the HUD.
//
// lavender_ui_update_interval N > 1 runs that update on one frame in N and
// skips the others, passing the skipped frames' time to the next update so
// animations and timers keep their speed. The HUD then moves at 1/N of the
// frame rate.
//
// The custom renderer controls (CCustomRendererCtrl's submit, sub_829435A0;
// FH1 sub_827DAA40, aligned, the same 30 instructions) submit their drawing
// every frame from inside the update, and a frame without that submission
// shows garbage where the minimap goes. The submission reads only the
// control's own fields, so on skipped frames each control seen in the last
// full update is submitted again on its own.
//
// Pinyon measured one frame in two as 0.46-0.70 ms of CPU a frame off the
// simulation thread on the Steam Deck and within noise on the Odin 2, so
// every frame stays the default. GRAPHICS > HUD UPDATES sets it.

#include <array>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "guest_native/guest_native.h"

REXCVAR_DEFINE_UINT32(lavender_ui_update_interval,
                      1, "Lavender Shift",
                      "Run the user interface update on one frame in N, carrying the "
                      "skipped frames' time; custom renderer controls such as the minimap "
                      "still draw every frame (1 = every frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" REX_FUNC(__imp__sub_829435A0);

namespace lavender_shift::guest_native {
namespace {

uint32_t Interval() { return REXCVAR_GET(lavender_ui_update_interval); }
bool Enabled() { return Interval() > 1; }

// Only the simulation thread runs the update and the controls' submissions.
constexpr size_t kMaxControls = 16;
struct State {
  uint32_t ui_object = 0;
  uint32_t skipped = 0;
  double carried = 0.0;
  // Controls submitted during the last full update, and the one running.
  std::array<uint32_t, kMaxControls> controls{};
  size_t control_count = 0;
  std::array<uint32_t, kMaxControls> recording{};
  size_t recording_count = 0;
  bool recording_open = false;
};
State state;

void CloseRecording() {
  if (!state.recording_open) return;
  state.recording_open = false;
  state.controls = state.recording;
  state.control_count = state.recording_count;
}

bool UiUpdateNative(PPCContext& ctx, uint8_t* base) {
  CloseRecording();
  if (ctx.r3.u32 != state.ui_object) {
    state = State{};
    state.ui_object = ctx.r3.u32;
  }
  if (state.control_count != 0 && state.skipped + 1 < Interval()) {
    ++state.skipped;
    state.carried += ctx.f1.f64;
    for (size_t i = 0; i < state.control_count; ++i) {
      PPCContext call = ctx;
      call.r3.u64 = state.controls[i];
      __imp__sub_829435A0(call, base);
    }
    return true;  // the rest of the update waits for the next full frame
  }
  // A full update with the summed time; it records the custom renderers.
  ctx.f1.f64 += state.carried;
  state.skipped = 0;
  state.carried = 0.0;
  state.recording_count = 0;
  state.recording_open = true;
  return false;
}

bool CustomRendererNative(PPCContext& ctx, uint8_t* /*base*/) {
  if (state.recording_open && state.recording_count < kMaxControls) {
    const uint32_t control = ctx.r3.u32;
    bool seen = false;
    for (size_t i = 0; i < state.recording_count; ++i) seen |= state.recording[i] == control;
    if (!seen) state.recording[state.recording_count++] = control;
  }
  return false;  // the original submits
}

Replacement ui_update_replacement = {
    .name = "ui.update_interval",
    .guest_address = 0x83000358u,
    .guest_code_size = 600,
    .guest_code_fnv1a = 0xA238C3738F683233ull,
    .returns = kReturnNone,
    .enabled = Enabled,
    .native = UiUpdateNative,
    .written = nullptr,
};

Replacement custom_renderer_replacement = {
    .name = "ui.custom_renderer_submit",
    .guest_address = 0x829435A0u,
    .guest_code_size = 120,
    .guest_code_fnv1a = 0x9499E9492C857248ull,
    .returns = kReturnNone,
    .enabled = Enabled,
    .native = CustomRendererNative,
    .written = nullptr,
};

}  // namespace
}  // namespace lavender_shift::guest_native

LAVENDER_GUEST_NATIVE(83000358, ::lavender_shift::guest_native::ui_update_replacement)
LAVENDER_GUEST_NATIVE(829435A0, ::lavender_shift::guest_native::custom_renderer_replacement)
