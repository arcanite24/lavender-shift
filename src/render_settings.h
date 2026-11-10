#pragma once

#include <cstddef>

namespace lavender_shift::render_settings {

// Brings the render scenarios to lavender_shadows and lavender_env_map_rate;
// called once per simulation step.
void Apply();
// Gameplay render scenarios recorded so far.
size_t ScenarioCount();
// Seconds since midnight in game, or -1 when there is no world.
double SecondsOfDay();

}  // namespace lavender_shift::render_settings
