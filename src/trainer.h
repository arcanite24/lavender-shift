#pragma once

#include <cstdint>
#include <optional>

// The trainer (docs/PINYON_PARITY.md LP-2.12), Pinyon Shift's live credits
// and time of day. The CHEATS page adds credits through the title's own
// accessors (the change waits until the player's profile is loaded) and
// holds the time of day at an hour or sets how long a day lasts.
namespace lavender_shift::trainer {

// Queues `amount` credits (negative takes them away; the balance stays
// between 0 and 999,999,999). Any thread.
void AddCredits(int64_t amount);
// The balance as last read, once a profile is loaded.
std::optional<uint32_t> Credits();
// On the simulation thread, each step of `delta_seconds`: applies a waiting
// credits change, reads the balance about once a second and holds or paces
// the time of day (lavender_cheat_time_of_day, lavender_cheat_day_length_minutes).
void Update(double delta_seconds);

}  // namespace lavender_shift::trainer
