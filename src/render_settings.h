#pragma once

#include <cstddef>
#include <cstdint>

namespace lavender_shift::render_settings {

// Brings the render scenarios to lavender_shadows and lavender_env_map_rate;
// called once per simulation step.
void Apply();
// Gameplay render scenarios recorded so far.
size_t ScenarioCount();
// Seconds since midnight in game, or -1 when there is no world.
double SecondsOfDay();
// The world as last passed to its time-of-day accessor, or 0 before free
// roam.
uint32_t World();

}  // namespace lavender_shift::render_settings
