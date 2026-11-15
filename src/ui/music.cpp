// Your Music, Pinyon Shift's src/ui/music.cpp with Lavender's names. The
// SDK's host player plays the files; the SDK's XMP app reports the system
// player as the playback controller while it plays, which FH2 answers by
// muting its radio (docs/PINYON_PARITY.md LP-2.8).

#include "ui/music.h"

#include <algorithm>
#include <atomic>
#include <cctype>

#include <rex/audio/host_music.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>

#include "diagnostics.h"

REXCVAR_DEFINE_BOOL(lavender_music, false, "Lavender Shift",
                    "Your music: play the MP3 and WAV files in lavender_music_folder instead "
                    "of the game's radio, which the game then mutes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(lavender_music_folder, "", "Lavender Shift",
                      "The folder (and its subfolders) your music plays from; empty: "
                      "<user>/music")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_music_shuffle, false, "Lavender Shift",
                    "Play your music in a random order")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(lavender_music_volume, 80, "Lavender Shift",
                     "Your music's volume, 0 to 100")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::music {

namespace {

std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}

// The note under the AUDIO page fits the page's width.
constexpr size_t kStatusWidth = 52;

std::string Fit(std::string text) {
  if (text.size() > kStatusWidth) text = text.substr(0, kStatusWidth - 3) + "...";
  return text;
}

// The user folder's own music folder by name; another one by its last two
// parts.
std::string ShortFolder() {
  if (REXCVAR_GET(lavender_music_folder).empty()) return "THE USER FOLDER'S MUSIC FOLDER";
  const auto folder = Folder();
  const auto parent = folder.parent_path().filename();
  return Upper(rex::path_to_utf8(parent.empty() ? folder.filename()
                                               : parent / folder.filename()));
}

std::filesystem::path g_user_root;
std::filesystem::path g_playing_folder;
std::atomic<bool> g_in_game{false};
std::function<void(std::function<void()>)> g_post_to_ui;
constexpr const char* kSettings[] = {"lavender_music", "lavender_music_folder",
                                     "lavender_music_shuffle", "lavender_music_volume"};

void Apply() {
  auto& player = rex::audio::HostMusicPlayer::Get();
  player.SetVolume(float(REXCVAR_GET(lavender_music_volume)) / 100.0f);
  player.SetShuffle(REXCVAR_GET(lavender_music_shuffle));
  if (!REXCVAR_GET(lavender_music) || !g_in_game.load(std::memory_order_acquire)) {
    if (!g_playing_folder.empty()) {
      player.Stop();
      g_playing_folder.clear();
    }
    return;
  }
  const auto folder = Folder();
  if (folder == g_playing_folder) return;
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  g_playing_folder = folder;
  const bool playing = player.Play(folder);
  diagnostics::RecordEvent("music.started",
                           {{"folder", rex::path_to_utf8(folder)},
                            {"tracks", std::to_string(player.GetStatus().count)},
                            {"playing", playing ? "1" : "0"}});
}

}  // namespace

std::filesystem::path Folder() {
  const std::string folder = REXCVAR_GET(lavender_music_folder);
  return folder.empty() ? g_user_root / "music" : rex::to_path(folder);
}

void Install(const std::filesystem::path& user_root,
             std::function<void(std::function<void()>)> post_to_ui) {
  g_user_root = user_root;
  g_post_to_ui = post_to_ui;
  rex::audio::HostMusicPlayer::Get().SetActiveCallback([](bool active) {
    if (auto* xmp = rex::kernel::xam::apps::XmpApp::Get()) {
      xmp->SetHostPlayback(active);
    }
    diagnostics::RecordEvent("music.active", {{"active", active ? "1" : "0"}});
  });
  for (const char* name : kSettings) {
    rex::cvar::RegisterChangeCallback(name, [post_to_ui](std::string_view, std::string_view) {
      post_to_ui(Apply);
    });
  }
  Apply();
}

void SetInGame(bool in_game) {
  if (g_in_game.exchange(in_game, std::memory_order_acq_rel) == in_game) return;
  diagnostics::RecordEvent("music.in_game", {{"in_game", in_game ? "1" : "0"}});
  if (g_post_to_ui) g_post_to_ui(Apply);
}

void Shutdown() {
  g_post_to_ui = nullptr;
  for (const char* name : kSettings) rex::cvar::UnregisterChangeCallbacks(name);
  auto& player = rex::audio::HostMusicPlayer::Get();
  player.SetActiveCallback(nullptr);
  player.Close();
  g_playing_folder.clear();
}

std::string StatusLine() {
  if (!REXCVAR_GET(lavender_music)) {
    return Fit("MP3 AND WAV FILES FROM " + ShortFolder());
  }
  if (!g_in_game.load(std::memory_order_acquire)) {
    return Fit("PLAYS IN GAME, FROM " + ShortFolder());
  }
  const auto status = rex::audio::HostMusicPlayer::Get().GetStatus();
  if (!status.count) {
    return Fit("NO MP3 OR WAV FILES IN " + ShortFolder());
  }
  return Fit(std::string(status.paused ? "PAUSED " : "PLAYING ") +
             std::to_string(status.index + 1) + "/" + std::to_string(status.count) + ": " +
             Upper(status.title));
}

void Next() {
  if (REXCVAR_GET(lavender_music)) rex::audio::HostMusicPlayer::Get().Next();
}

void Previous() {
  if (REXCVAR_GET(lavender_music)) rex::audio::HostMusicPlayer::Get().Previous();
}

}  // namespace lavender_shift::music
