#pragma once

#include <functional>

namespace lavender_shift::wide_view {
// The horizontal widening for the window's aspect (1 = 16:9 or off).
void SetAspectScale(float scale);
// Once per simulation step: decides whether the frame is the gameplay view
// (widened, stretched) or one of the game's 16:9 screens (letterboxed).
void Update();
// Called on a change between the two, with true for a 16:9 screen.
void SetNarrowCallback(std::function<void(bool narrow)> callback);
}  // namespace lavender_shift::wide_view
