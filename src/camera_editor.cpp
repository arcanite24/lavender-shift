// The chase camera editor (docs/PINYON_PARITY.md LP-2.6), Pinyon Shift's,
// with FH2 TU v2 addresses mapped from FH1 v4 by Pinyon's address mapper:
// - 0x82496CF0(world): the number of camera controllers (FH1 v4 0x824B68E0,
//   context);
// - 0x82496D20(world, i): controller i (FH1 v4 0x824B6910, context);
// - 0x829C5018(controller, view): switches its view (FH1 v4 0x82908AC0,
//   exact), so a controller keeps its view at +8 and one camera per view
//   from +24, as in FH1.
// The world is [0x8248F578([0x83422224], 0)], as in ai_driver.cpp.
//
// The two chase cameras (views 0 and 1) point at +192 to their
// CameraSettings.ini FollowLowCam / FollowHighCam block, title data loaded
// once. FH2's block is 16 floats, not FH1's 14 (found with
// lavender_camera_probe in free roam; docs/findings/systems.toml):
//   0 height, 1 radius, 2 target height, 3 (0), 4/5 min/max height above
//   ground, 6/7 near/far clip, 8 FOV in degrees, 9/10 yaw spring K/D,
//   11 yaw damping scale, 12 yaw max speed, 13 height spring K, 14 (10),
//   15 height spring D.
// Each camera sets its height, radius and target height at +268/+276/+280
// when it starts, from the block and the car's own values, so height and
// distance are offsets on those, as in Pinyon (the target stays, so a higher
// camera looks down at the car). FOV and stiffness edit the block, always
// from the stock values first read, never from an earlier edit.

#include "camera_editor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include "diagnostics.h"
#include "guest_memory.h"

REXCVAR_DEFINE_DOUBLE(lavender_chase_height, 0.0, "Lavender Shift",
                      "Chase camera height offset in metres")
    .range(-0.5, 1.5);
REXCVAR_DEFINE_DOUBLE(lavender_chase_distance, 0.0, "Lavender Shift",
                      "Chase camera distance offset in metres")
    .range(-1.0, 3.0);
REXCVAR_DEFINE_DOUBLE(lavender_chase_fov, 1.0, "Lavender Shift",
                      "Chase camera field of view multiplier")
    .range(0.8, 1.3);
REXCVAR_DEFINE_DOUBLE(lavender_chase_stiffness, 1.0, "Lavender Shift",
                      "Chase camera yaw and height spring stiffness multiplier")
    .range(0.5, 2.0);
REXCVAR_DEFINE_BOOL(lavender_camera_probe, false, "Lavender Shift",
                    "Diagnostic: log the camera controllers and chase-camera tuning data");

namespace {

constexpr uint32_t kGame = 0x83422224u;
constexpr uint32_t kWorldOf = 0x8248F578u;
constexpr uint32_t kCameraCount = 0x82496CF0u;
constexpr uint32_t kCameraController = 0x82496D20u;

constexpr uint32_t kControllerCameras = 24;
constexpr uint32_t kCameraCar = 16;
constexpr uint32_t kCameraTuning = 192;
constexpr uint32_t kCameraHeight = 268;
constexpr uint32_t kCameraRadius = 276;

using Tuning = std::array<float, 16>;
constexpr uint32_t kFov = 8;

// Height is not edited here, unlike Pinyon's: in FH2 a camera that starts
// after an edit of 4 and 5 takes it into its own height and target (+268,
// +280), so the offset was counted twice and outlived a reset.
Tuning Adjust(Tuning stock, float fov, float stiffness) {
  stock[kFov] *= fov;
  // The same damping ratio at the new spring frequency.
  stock[9] *= stiffness;
  stock[10] *= std::sqrt(stiffness);
  stock[13] *= stiffness;
  stock[15] *= std::sqrt(stiffness);
  return stock;
}

// An offset on a camera value the title may replace at any time (a new car
// or a camera reset): a value other than the last one written is the new
// base.
struct Offset {
  float base = 0, written = 0;
  bool initialized = false;

  float Apply(float current, float offset) {
    if (!initialized || current != written) base = current;
    initialized = true;
    written = base + offset;
    return written;
  }
};

uint32_t Pointer(uint32_t field, uint32_t size) {
  using namespace lavender_shift::guest;
  if (!Readable(field, 4)) return 0;
  const uint32_t value = LoadU32(field);
  return value && Readable(value, size) ? value : 0;
}

uint32_t Call(uint32_t address, uint32_t a0, uint32_t a1 = 0) {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->function_dispatcher()->GetFunction(address)) return 0;
  uint64_t args[6] = {a0, a1, 0, 0, 0, 0};
  return uint32_t(kernel_state->function_dispatcher()->ExecuteTrap(
      rex::runtime::ThreadState::Get(), address, args, 6));
}

std::string Floats(uint32_t address, uint32_t count) {
  std::string out;
  for (uint32_t n = 0; n < count; ++n) {
    if (!lavender_shift::guest::Readable(address + 4 * n, 4)) break;
    out += fmt::format("{}{:g}", n ? " " : "", lavender_shift::guest::LoadF32(address + 4 * n));
  }
  return out;
}

void Probe(uint32_t world) {
  using namespace lavender_shift::guest;
  const uint32_t count = Call(kCameraCount, world);
  REXLOG_INFO("Camera probe: world {:08X}, {} controllers", world, count);
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t controller = Call(kCameraController, world, i);
    if (!controller || !Readable(controller, 120)) continue;
    std::string cameras;
    for (uint32_t n = 0; n < 24; ++n) {
      cameras += fmt::format(" {:08X}", LoadU32(controller + kControllerCameras + 4 * n));
    }
    REXLOG_INFO("Camera probe: controller {} at {:08X}, view {}, cameras{}", i, controller,
                LoadU32(controller + 8), cameras);
    for (uint32_t chase = 0; chase < 2; ++chase) {
      const uint32_t camera = Pointer(controller + kControllerCameras + 4 * chase, 320);
      if (!camera) continue;
      const uint32_t tuning = Pointer(camera + kCameraTuning, 64);
      REXLOG_INFO("Camera probe:  chase {} at {:08X}, car {:08X}, tuning {:08X}: {}", chase,
                  camera, LoadU32(camera + kCameraCar), tuning, tuning ? Floats(tuning, 16) : "");
      REXLOG_INFO("Camera probe:  chase {} +256..+320: {}", chase, Floats(camera + 256, 16));
    }
  }
}

void Edit(uint32_t world, const std::array<float, 4>& values) {
  using namespace lavender_shift::guest;
  // Tuning blocks are title data: their stock values hold for the session.
  static std::map<uint32_t, Tuning> originals;
  struct Placement {
    uint32_t car = 0;
    std::array<Offset, 2> offsets;
  };
  static std::map<uint32_t, Placement> placements;
  static std::array<float, 4> reported{0, 0, 1, 1};

  const uint32_t count = Call(kCameraCount, world);
  for (uint32_t i = 0; i < std::min(count, 4u); ++i) {
    const uint32_t controller = Call(kCameraController, world, i);
    if (!controller || !Readable(controller, 120)) continue;
    for (uint32_t chase = 0; chase < 2; ++chase) {
      const uint32_t camera = Pointer(controller + kControllerCameras + 4 * chase, 288);
      if (!camera) continue;
      const uint32_t tuning = Pointer(camera + kCameraTuning, 64);
      if (!tuning) continue;
      auto found = originals.find(tuning);
      if (found == originals.end()) {
        Tuning stock;
        for (uint32_t n = 0; n < stock.size(); ++n) stock[n] = LoadF32(tuning + 4 * n);
        // Not a chase block: leave it alone.
        if (!std::all_of(stock.begin(), stock.end(), [](float v) { return std::isfinite(v); }) ||
            stock[kFov] < 1 || stock[kFov] > 180) {
          continue;
        }
        found = originals.emplace(tuning, stock).first;
        lavender_shift::diagnostics::RecordEvent(
            "camera.editor.data", {{"chase", std::to_string(chase)},
                                   {"height", fmt::format("{:g}", stock[0])},
                                   {"distance", fmt::format("{:g}", stock[1])},
                                   {"fov", fmt::format("{:g}", stock[kFov])},
                                   {"yaw_k", fmt::format("{:g}", stock[9])},
                                   {"height_k", fmt::format("{:g}", stock[13])}});
      }
      const Tuning adjusted = Adjust(found->second, values[2], values[3]);
      for (const uint32_t n : {kFov, 9u, 10u, 13u, 15u}) {
        StoreF32(tuning + 4 * n, adjusted[n]);
      }
      // The far chase camera has no car until it is first used, and its
      // placement is zero until then.
      const uint32_t car = LoadU32(camera + kCameraCar);
      if (!car) continue;
      const float height = LoadF32(camera + kCameraHeight);
      const float radius = LoadF32(camera + kCameraRadius);
      if (!std::isfinite(height) || !std::isfinite(radius)) continue;
      auto& placement = placements[camera];
      if (placement.car != car) placement = Placement{car, {}};
      StoreF32(camera + kCameraHeight, placement.offsets[0].Apply(height, values[0]));
      StoreF32(camera + kCameraRadius, placement.offsets[1].Apply(radius, values[1]));
    }
  }
  if (reported != values) {
    reported = values;
    lavender_shift::diagnostics::RecordEvent(
        "camera.editor.applied", {{"height", fmt::format("{:g}", values[0])},
                                  {"distance", fmt::format("{:g}", values[1])},
                                  {"fov", fmt::format("{:g}", values[2])},
                                  {"stiffness", fmt::format("{:g}", values[3])}});
  }
}

}  // namespace

namespace lavender_shift::camera_editor {

void Update() {
  static uint32_t steps = 0;
  ++steps;
  const bool probe = REXCVAR_GET(lavender_camera_probe) && steps % 600 == 0;
  // Five times a second at 60: a new car or camera reset is picked up soon.
  if (!probe && steps % 12) return;
  const std::array<float, 4> values = {
      float(REXCVAR_GET(lavender_chase_height)), float(REXCVAR_GET(lavender_chase_distance)),
      float(REXCVAR_GET(lavender_chase_fov)), float(REXCVAR_GET(lavender_chase_stiffness))};
  // Untouched until the player first changes a value.
  static bool used = false;
  if (!used && values == std::array<float, 4>{0, 0, 1, 1} && !probe) return;
  if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); })) return;
  const uint32_t game = Pointer(kGame, 8);
  const uint32_t where = game ? Call(kWorldOf, game, 0) : 0;
  const uint32_t world = where ? Pointer(where, 136) : 0;
  if (!world) return;
  if (probe) Probe(world);
  if (used || values != std::array<float, 4>{0, 0, 1, 1}) {
    used = true;
    Edit(world, values);
  }
}

}  // namespace lavender_shift::camera_editor
