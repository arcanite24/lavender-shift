// Starting lavender_shift.exe on its own (double-clicked, from a shortcut or
// from the launcher) with no arguments: the game finds the folders setup put
// beside it, as tools/launch.ps1 passes them.
#pragma once

#include <filesystem>

#include <rex/rex_app.h>

namespace lavender_shift::install {

// When nothing named the game's folder, look for setup's layout around the
// executable (out/build/<preset>/lavender_shift.exe in the source folder):
// the disc and update in .local/game, and the player's folder in user\ beside
// the source folder in a release (where lavender-shift-source.zip is) or in
// .local/user in a checkout. The flags launch.ps1 passes are set too, unless
// the command line set them. Returns whether it filled the folders.
bool ResolveDefaults(rex::PathConfig& paths);

// One game per player folder: false when another instance already runs on
// `user_root` (Windows; elsewhere always true).
bool ClaimUserRoot(const std::filesystem::path& user_root);

// Tells the player the game is already open (a message box on Windows).
void ShowAlreadyRunning();

}  // namespace lavender_shift::install
