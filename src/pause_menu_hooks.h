#pragma once

#include <functional>

namespace lavender_shift::pause_menu {

// What the pause menu's SETTINGS tile runs (on the guest thread that handles
// the menu's input): post the host settings menu to the UI thread. Null
// leaves the game's own tile behaviour.
void SetSettingsHandler(std::function<void()> handler);

}  // namespace lavender_shift::pause_menu
