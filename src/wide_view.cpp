// WIDER VIEW, Hor+ for windows wider than 16:9 (docs/PINYON_PARITY.md,
// LP-2.4), as Pinyon Shift's NP-4.4.
//
// FH2's viewport aspect getter (TU v2 0x823F2328, its return at 0x823F2354)
// returns a viewport's width / height (r3 the viewport, width and height at
// +8 and +12) to the camera, its culling and its projection. It is FH1 v4's
// sub_8241228C instruction for instruction, the only such function in the
// image. For the main view a wider window's aspect is returned instead, so
// the game renders and culls a wider horizontal field into the same 16:9
// image, which the presenter stretches over the window and the SDK's HUD
// squeeze keeps in proportion.
//
// Every 1280x720 view is widened except the pause map's, which draws a
// 16:9 layer. Two of the getter's seven callers are told apart by the return
// address (the mapper's matches for FH1 v4's, each checked to call the
// getter): the gameplay camera's view (0x82B24824, FH1 v4 0x82A58E34) marks
// the frames that are stretched, and the pause map's (0x82789EC0, FH1 v4
// 0x826E30A8) is left alone. Frames without the gameplay view (menus,
// loading, the map) are shown letterboxed instead of stretched.

#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "wide_view.h"

namespace {
constexpr uint32_t kGameProjectionReturn = 0x82B24824u;
constexpr uint32_t kMapProjectionReturn = 0x82789EC0u;

std::atomic<float> g_scale{1.0f};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_game_view_frame{0};
std::atomic<uint64_t> g_map_view_frame{0};
std::atomic<bool> g_narrow{true};
std::mutex g_callback_mutex;
std::function<void(bool)> g_callback;
std::atomic<uint32_t> g_main_width{0};
std::atomic<uint32_t> g_main_height{0};
}  // namespace

void LavenderViewportAspect(PPCContext& context, [[maybe_unused]] uint8_t* base, PPCRegister& r3,
                            PPCRegister& f1) {
  const uint32_t lr = uint32_t(context.lr);
  const uint64_t frame = g_frames.load(std::memory_order_relaxed);
  if (lr == kMapProjectionReturn) {
    g_map_view_frame.store(frame, std::memory_order_relaxed);
    return;
  }
  const uint32_t width = lavender_shift::guest::LoadU32(r3.u32 + 8u);
  const uint32_t height = lavender_shift::guest::LoadU32(r3.u32 + 12u);
  if (lr == kGameProjectionReturn) {
    g_game_view_frame.store(frame, std::memory_order_relaxed);
    if (g_main_width.exchange(width, std::memory_order_relaxed) != width ||
        g_main_height.exchange(height, std::memory_order_relaxed) != height) {
      REXLOG_INFO("Gameplay view {}x{}", width, height);
    }
  }
  // Every caller asking for the main view's aspect (the camera, its culling
  // and its projection come through different callers); render-to-texture
  // views (car thumbnails, reflections) keep their own size and aspect.
  const float scale = g_scale.load(std::memory_order_relaxed);
  if (scale == 1.0f || g_narrow.load(std::memory_order_relaxed) || width != 1280u ||
      height != 720u) {
    return;
  }
  f1.f64 = double(float(f1.f64) * scale);
}

namespace lavender_shift::wide_view {

void SetAspectScale(float scale) { g_scale.store(scale, std::memory_order_relaxed); }

void SetNarrowCallback(std::function<void(bool)> callback) {
  std::lock_guard lock(g_callback_mutex);
  g_callback = std::move(callback);
}

void Update() {
  // The map switches at once; leaving the gameplay view otherwise takes half
  // a second, so a dialog or a camera cut does not flicker. Pinyon's rule.
  static bool narrow = true;
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t map = g_map_view_frame.load(std::memory_order_relaxed);
  const uint64_t game = g_game_view_frame.load(std::memory_order_relaxed);
  const bool game_recent = game != 0 && frame - game <= 2;
  const bool map_recent = map != 0 && frame - map <= 3;
  const bool game_gone = game == 0 || frame - game > 30;
  const bool now_narrow = narrow ? !game_recent : (game_gone || (map_recent && frame - game > 3));
  if (now_narrow == narrow) return;
  narrow = now_narrow;
  g_narrow.store(narrow, std::memory_order_relaxed);
  std::function<void(bool)> callback;
  {
    std::lock_guard lock(g_callback_mutex);
    callback = g_callback;
  }
  if (callback) callback(narrow);
}

}  // namespace lavender_shift::wide_view
