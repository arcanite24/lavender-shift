// The simulation step's time delta. FH2 stores each step's delta, in
// seconds, at owner+448 (TU v2 0x823FDEF4, the same store as FH1 v4
// 0x8241DC44; mapped by Pinyon's address mapper and checked by the store
// instruction). The hook feeds the SDK's simulation counters, so the perf
// CSV shows steps per second and simulated seconds per second.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>
#include <rex/thread.h>

#include "ai_driver.h"
#include "dev_options.h"
#include "graphics_hooks.h"
#include "guest_memory.h"
#include "render_settings.h"
#include "simulation_hooks.h"
#include "stall_dump.h"
#include "wide_view.h"

void LavenderObserveSimulationDelta(PPCRegister& f31) {
  const double seconds = f31.f64;
  if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 0.25) {
    PROFILE_SIMULATION_DELTA_INVALID();
    return;
  }
  PROFILE_SIMULATION_TICK();
  PROFILE_SIMULATION_TIME_NS(int64_t(std::llround(seconds * 1e9)));
  lavender_shift::graphics::UpdateHud();
  lavender_shift::graphics::ApplyMotionBlur();
  lavender_shift::render_settings::Apply();
  lavender_shift::dev_options::Update();
  lavender_shift::wide_view::Update();
  lavender_shift::ai_driver::Update();
  lavender_shift::graphics::LogHookCalls();
  lavender_shift::stall::NoteFrame();
}

// FH2's render job queue keeps its producer at most twice its threshold ahead
// of the job thread by yielding, then gives up after 1000 yields and posts
// anyway (counter at r1+84). On the 360 those yields outlast a render-thread
// stall; on the host a yield returns at once, and in FH1 a slow GPU frame let
// the main thread queue hundreds of jobs, overrun the render thread's slots
// and crash or hang. Pinyon Shift's fix, at the same site (TU v2 0x82405248,
// FH1 v4 0x82424FB0, identical instructions): before each yield after the
// first pass, sleep 50 us and hold the counter up, for at most two seconds.
REXCVAR_DEFINE_BOOL(lavender_throttle_render_jobs, true, "Lavender Shift",
                    "Wait in host time when the render job queue is full, as the 360's "
                    "yields did, instead of overrunning the render thread");

void LavenderThrottleRenderJobs(PPCRegister& r1) {
  if (!REXCVAR_GET(lavender_throttle_render_jobs)) return;
  using Clock = std::chrono::steady_clock;
  constexpr uint32_t kIterations = 1000;
  constexpr auto kLimit = std::chrono::seconds(2);
  thread_local Clock::time_point started;
  const uint32_t counter = r1.u32 + 84;
  const uint32_t left = lavender_shift::guest::LoadU32(counter);
  const Clock::time_point now = Clock::now();
  if (left >= kIterations) {
    started = now;
    return;  // The first pass yields as before; usually that is enough.
  }
  if (now - started < kLimit) {
    if (left < 2) lavender_shift::guest::StoreU32(counter, 2);
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

// The swap interrupt targets the next vblank when the swap arrives more than
// its threshold (20 % of a display period) after the latest vblank. When two
// guest vblanks (one frame at 60) have already passed since the previous
// swap's target, the frame is late anyway: report it as on time (r7, the
// percent late, to 0) so it flips now instead of a vblank later. r8 is the
// previous target, r10 the current vblank count. Pinyon Shift's DR-5.2, at
// the same site (TU v2 0x82B71844, FH1 v4 0x82AABDB4, identical).
REXCVAR_DEFINE_BOOL(lavender_release_late_swaps, true, "Lavender Shift",
                    "Flip a frame that is already a whole frame late at once instead of "
                    "holding it for the next vblank");

void LavenderReleaseLateSwap(PPCRegister& r7, PPCRegister& r8, PPCRegister& r10) {
  if (REXCVAR_GET(lavender_release_late_swaps) && r10.u32 - r8.u32 >= 2) {
    r7.u64 = 0;
  }
}

// The render thread's idle path: when the simulation has no new frame for
// it, its frame function rebuilds the gamma ramp, reads the time and calls
// SleepEx(0, TRUE), which returns at once on the host, so the path runs
// back to back. Before that call (TU v2 0x82618530, the call to the SleepEx
// wrapper 0x82BF0D50; FH1 v4 0x82616CD0 and 0x82B2A848, aligned and exact),
// wait on a host timer first; the guest call then delivers any APC that
// arrived meanwhile. Pinyon Shift's #431: the Odin render thread's CPU fell
// from 477-839 to 242-253 ms a second.
REXCVAR_DEFINE_UINT32(lavender_render_idle_sleep_us, 200, "Lavender Shift",
                      "Microseconds the render thread sleeps when it has no new frame to draw "
                      "(0 = the game's own spin)");

namespace {
std::atomic<uint64_t> g_render_idle_calls{0};
}

void LavenderRenderIdleSleep() {
  g_render_idle_calls.fetch_add(1, std::memory_order_relaxed);
  if (const uint32_t us = REXCVAR_GET(lavender_render_idle_sleep_us)) {
    rex::thread::WaitUntil(std::chrono::steady_clock::now() + std::chrono::microseconds(us));
  }
}

namespace lavender_shift::simulation {
uint64_t RenderIdleCalls() { return g_render_idle_calls.load(std::memory_order_relaxed); }
}  // namespace lavender_shift::simulation
