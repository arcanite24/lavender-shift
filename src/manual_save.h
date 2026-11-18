#pragma once

#include <string>

namespace lavender_shift::manual_save {

// Asks for the title's own full profile save (the one events and the garage
// use) at the start of the next UI update. Safe from any thread.
void Request();

// Called at the start of the title's UI update (src/guest_native/ui_update.cpp)
// while a save is pending: runs it there.
void Update();

// Whether a save is requested or waiting for the title to allow it.
bool Pending();

// What the last request did, for the menu: empty before the first one.
std::string Status();

}  // namespace lavender_shift::manual_save
