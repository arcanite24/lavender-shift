#pragma once

#include <filesystem>

namespace lavender_shift::mods {

// The immersive camera (docs/PINYON_PARITY.md LP-2.6), Pinyon Shift's extra
// camera-physics layers: G-forces, the throttle and brake jolt, corner lean,
// speed shake, braking vibration, breathing and head sway. At start the game
// rebuilds the disc's media/camera.zip with the layers appended to
// CameraPhysics.xml (src/mods/immersive_camera_layers.inc, written by
// tools/build-immersive-camera.py) into `cache_root`/immersive_camera, and
// serves it ahead of any mod through the overlay device. The archive is
// built again when the disc's changes. The IMMERSIVE CAMERA rows scale each
// effect group live (LavenderImmersiveCameraLayer); at 0 a group is off.
//
// Returns the folder to serve as a game root (its media/camera.zip), or an
// empty path when lavender_immersive_camera is off or the build failed.
std::filesystem::path BuildImmersiveCamera(const std::filesystem::path& game_data_root,
                                           const std::filesystem::path& cache_root);

}  // namespace lavender_shift::mods
