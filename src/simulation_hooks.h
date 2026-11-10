#pragma once

#include <cstdint>

namespace lavender_shift::simulation {
// Calls of the render thread's idle sleep so far (src/simulation_hooks.cpp).
uint64_t RenderIdleCalls();
}  // namespace lavender_shift::simulation
