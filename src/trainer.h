#pragma once

#include <cstdint>
#include <optional>

// The trainer's credits (docs/PINYON_PARITY.md LP-2.12), Pinyon Shift's live
// credits through the title's own accessors. The CHEATS page adds credits;
// the change waits until the player's profile is loaded and applies on the
// next simulation step.
namespace lavender_shift::trainer {

// Queues `amount` credits (negative takes them away; the balance stays
// between 0 and 999,999,999). Any thread.
void AddCredits(int64_t amount);
// The balance as last read, once a profile is loaded.
std::optional<uint32_t> Credits();
// On the simulation thread, each step: applies a waiting change and reads
// the balance about once a second.
void Update();

}  // namespace lavender_shift::trainer
