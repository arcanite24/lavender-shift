#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace lavender_shift::music {

// Your Music (docs/PINYON_PARITY.md LP-2.8, Pinyon Shift's #420): the
// player's own MP3 and WAV files play on the host instead of the game's
// radio. Turning it on tells the game the system's music player has the
// music, as a 360 dashboard soundtrack did, so the game mutes its own radio
// by its own path.
//
// Applies the lavender_music* settings now and whenever they change;
// `post_to_ui` runs a task on the UI thread. `user_root` holds the default
// folder, <user>/music.
void Install(const std::filesystem::path& user_root,
             std::function<void(std::function<void()>)> post_to_ui);
// Whether the game has a world (free roam, events, the festival's menus).
// The music plays only then, so the title screen keeps the game's own music;
// called from the simulation step.
void SetInGame(bool in_game);
// Stops the music and drops the callbacks, before the kernel goes away.
void Shutdown();

// The folder music is read from: lavender_music_folder, or <user>/music.
std::filesystem::path Folder();

// The line under the AUDIO page: what is playing, or how to add music.
std::string StatusLine();

void Next();
void Previous();

}  // namespace lavender_shift::music
