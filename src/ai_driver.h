#pragma once

#include <cstdint>

namespace lavender_shift::ai_driver {

// Called on the simulation thread every step: logs game-mode changes and
// applies the lavender_ai_drives_player diagnostic.
void Update();

// The last game mode read from the session (0xFFFFFFFF when none).
uint32_t GameMode();

}  // namespace lavender_shift::ai_driver
