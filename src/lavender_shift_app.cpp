#include "lavender_shift_app.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/keybinds.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "config/default_keys.h"
#include "platform/display_hdr.h"
#include "graphics_hooks.h"
#include "movie_hooks.h"
#include "wide_view.h"
#include "ui/photo_export.h"
#include "cvar_cycle.h"
#include "diagnostics.h"
#include "save_backups.h"
#include "pause_menu_hooks.h"
#include "mods/data_patches.h"
#include "mods/immersive_camera.h"
#include "mods/mods.h"
#include "mods/overlay_device.h"
#include "ui/frame_inspector.h"
#include "ui/music.h"
#include "ui/settings_menu.h"
#include "ui/xam_dialogs.h"
#include "guest_native/guest_native.h"
#include "frame_rate.h"
#include "install_paths.h"

LavenderShiftApp::~LavenderShiftApp() = default;

REXCVAR_DECLARE(std::string, gpu_backend);
REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(int32_t, monitor);

REXCVAR_DEFINE_INT32(lavender_master_volume, 100, "Audio",
                     "Master volume, 0 to 100, applied to the output mix")
    .range(0, 100);

namespace {
void ApplyMasterVolume() {
  // Squared so equal steps sound roughly even (Pinyon Shift's curve).
  const float linear = float(std::clamp(REXCVAR_GET(lavender_master_volume), 0, 100)) / 100.0f;
  rex::audio::SetOutputGain(linear * linear);
}
}  // namespace

REXCVAR_DEFINE_BOOL(lavender_host_xam_dialogs, true, "Lavender Shift",
                    "Draw the game's message boxes and keyboard with the host UI (the game's "
                    "fonts, pad navigation) instead of the SDK's ImGui dialogs")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(lavender_save_backups, true, "Lavender Shift",
                    "Copy the profile folders to <user>/backups/saves after the game saves (at "
                    "most every ten minutes, and at exit); the PROFILE page restores them")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(lavender_save_backup_count, 10, "Lavender Shift",
                     "Save backups to keep; older ones are deleted")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(lavender_sync_folder, "", "Lavender Shift",
                      "A folder a sync app (Syncthing, Dropbox, OneDrive...) keeps in step "
                      "between devices; the save and achievements are shared through it "
                      "(docs/SAVE_SYNC.md). Empty: off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(enabled_mods, "", "Mods",
                      "The mods in <user>/mods to load, comma-separated, earlier ones winning "
                      "(src/mods/mods.h)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(lavender_device_name, "", "Lavender Shift",
                      "This device's name as other devices show it in SAVE SYNC; empty uses "
                      "the computer's name")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#if !defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(fh1_frame_inspector, false, "GPU",
                    "Open the desktop frame inspector on startup (Ctrl+F8 toggles it)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_frame_inspector_replay, false, "GPU",
                    "Start a desktop frame-dump workspace with the title suspended")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_frame_inspector_view, 0, "GPU",
                     "Initial replay inspector view: 0 color, 1 wireframe, 2 overdraw")
    .range(0, 2);
#endif
REXCVAR_DEFINE_BOOL(lavender_windows_hdr, false, "Display",
                    "Windows: turn HDR on for the game's display while the game runs, and "
                    "back off when it closes (a display already in HDR is left on)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lavender_hor_plus, false, "Display",
                    "Ultrawide: widen the horizontal field of view to the window's aspect "
                    "(Hor+), shown stretched from the 16:9 image; the HUD keeps its shape")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
// WINDOWS HDR (Pinyon Shift's switch): HDR on for the display showing the
// window, or before the window exists for the primary display, which always
// holds the desktop's origin.
void TurnOnWindowsHdr(void* native_window) {
  const auto result = native_window ? lavender_shift::platform::TurnOnDisplayHdr(native_window)
                                    : lavender_shift::platform::TurnOnDisplayHdrAt(0, 0);
  lavender_shift::diagnostics::RecordEvent(
      "display.windows_hdr", {{"result", lavender_shift::platform::DisplayHdrResultName(result)}});
}
}  // namespace

// Stage 0 renders through ShiftGlue's Xenos (PM4) plugin, rexgpu-fh1, unless
// --gpu_plugin names another one.
void LavenderShiftApp::OnPreSetup(rex::RuntimeConfig& config) {
  // Before the guest reads its video mode (frame generation's render cap is
  // half of it).
  lavender_shift::frame_rate::PublishRefreshRate(window() ? window()->GetNativeWindowHandle()
                                                          : nullptr);
  // WINDOWS HDR: before the swapchain is made, so the presenter is offered
  // scRGB. A display a crashed run left in HDR is turned back off first. The
  // list lives in the player's own folder, also while a modded profile plays.
  lavender_shift::platform::SetDisplayHdrRestoreFile(photos_dir_.parent_path() / "cache" /
                                                     "windows_hdr_restore.txt");
  // On another MONITOR it waits for the window (OnPostSetup).
  if (REXCVAR_GET(lavender_windows_hdr)) {
    if (REXCVAR_GET(monitor) <= 0) TurnOnWindowsHdr(nullptr);
  } else if (const int restored = lavender_shift::platform::RestoreDisplayHdr()) {
    lavender_shift::diagnostics::RecordEvent(
        "display.windows_hdr",
        {{"result", "restored_after_crash"}, {"displays", std::to_string(restored)}});
  }
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "fh1";
  }
  // Loaded here rather than by the SDK just after, so its settings exist when
  // Lavender's defaults are set; on failure the SDK loads it again and reports.
  if (!config.graphics) {
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, REXCVAR_GET(gpu_backend));
  }
  // FH2 has no split screen and only user 0 is signed in, so every pad plays
  // as user 0, as in Pinyon Shift (its #553). With the SDK's slot assignment
  // a pad behind a virtual one (Steam Input, ViGEm) or a wheel pressed Start
  // as user 1 and FH2 showed "No Profiles".
  if (config.input_factory) {
    config.input_factory = [base = config.input_factory](bool tool_mode) {
      auto input = base(tool_mode);
      if (auto* system = dynamic_cast<rex::input::InputSystem*>(input.get())) {
        system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
      }
      return input;
    };
  }
  // Lavender's defaults where the SDK's differ, set once the GPU plugin has
  // registered its settings and before the runtime starts; a value from the
  // config file or the command line wins. Each game frame is shown exactly once, paced by
  // vsync: presenting on the host's own 60 Hz clock instead showed 8.4 repeated
  // and 9.0 dropped frames a second in free roam, because FH2 renders a frame
  // after a single 120 Hz guest vblank when the one before it ran late
  // (docs/STAGE0.md, "Frame pacing"). Keeping CPU-written pages between frames
  // took the free-roam route's process CPU from 1.58 to 1.36 cores with frame
  // times and pacing unchanged (docs/PINYON_PARITY.md, LP-1.2). Pinyon's
  // separate GPU recorder thread then cost 0.09 cores more, so it stays off
  // (LP-1.1).
  //
  // The motion blur's velocity pass reads each pixel's stencil as an object
  // ID, and its test for flag values (bit 7) starts just above 0x80, so the
  // road the car shadow's stencil volume marks with 0x80 took an identity
  // transform and stayed sharp: a square with no blur under moving cars.
  // A threshold of 0.5 counts 0x80 as a flag too (docs/STAGE0.md, "Motion blur").
  for (const auto& [name, value] :
       {std::pair{"pinyon_shift_fh1_source_presentation", "true"},
        std::pair{"host_present_fps_limit", "0"}, std::pair{"clear_memory_page_state", "false"},
        std::pair{"gpu_pixel_constant_fixups", "ADD2563BA387C2BF:253.w:3F008100=3F000000"}}) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
    }
  }
}

// The settings live with the player's data, in <user>/config, so each user
// root (a test copy included) keeps its own. The SDK loads the file before
// anything reads a setting; the settings menu edits it in place.
void LavenderShiftApp::OnConfigurePaths(rex::PathConfig& paths) {
  // Started on its own, with no folders named: the ones setup put beside it.
  const bool found_install = lavender_shift::install::ResolveDefaults(paths);
  if (!lavender_shift::install::ClaimUserRoot(paths.user_data_root)) {
    lavender_shift::install::ShowAlreadyRunning();
    std::exit(0);
  }
  // Crash reports, stall dumps and session events (LP-1.10) from here on.
  lavender_shift::diagnostics::Initialize(paths.user_data_root);
  // The gamepad mappings the build puts beside the game (CMakeLists.txt),
  // wherever it was started from; SDL would look in the current folder.
  if (rex::cvar::GetFlagSource("hid_mappings_file") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName(
        "hid_mappings_file",
        rex::path_to_utf8(rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt"));
  }
  if (found_install) {
    lavender_shift::diagnostics::RecordEvent(
        "install.found", {{"game", rex::path_to_utf8(paths.game_data_root)},
                          {"user", rex::path_to_utf8(paths.user_data_root)}});
  }
  paths.config_path = paths.user_data_root / "config" / "lavender_shift.toml";
  // A restore chosen on the PROFILE page, before the title reads its saves.
  lavender_shift::SaveBackups::ApplyPendingRestore(paths.user_data_root,
                                                   paths.user_data_root / "backups" / "saves");
  user_root_ = paths.user_data_root;
  // Save sync, also before the title reads its saves and the runtime its
  // achievement unlocks: a save another device continued from this one's
  // is put in place, and every device's unlocks are merged.
  // The settings file is loaded only after this, so its values are read
  // here when the command line did not set them.
  const auto setting = [&](const char* name, std::string value) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      if (auto text = lavender_shift::config::ReadFile(paths.config_path)) {
        value = lavender_shift::config::GetValue(*text, name).value_or(value);
      }
    }
    return value;
  };
  const std::string sync_folder = setting("lavender_sync_folder", REXCVAR_GET(lavender_sync_folder));
  if (!sync_folder.empty()) {
    save_sync_ = std::make_unique<lavender_shift::SaveSync>(
        paths.user_data_root, rex::to_path(sync_folder),
        setting("lavender_device_name", REXCVAR_GET(lavender_device_name)));
    sync_startup_ = save_sync_->Startup();
  }
  photos_dir_ = paths.user_data_root / "photos";
  std::error_code error;
  std::filesystem::create_directories(paths.config_path.parent_path(), error);
  if (!std::filesystem::exists(paths.config_path, error)) {
    config_created_ = true;
    std::ofstream file(paths.config_path);
    file << "# Lavender Shift settings, written by the in-game menu (F6).\n";
    // Keyboard and mouse beside any controller, as in Pinyon Shift: without
    // it a player with no controller cannot press Start at the title.
    file << "mnk_mode = true\n";
    // Driving-first keyboard keys (src/config/default_keys.h); older files
    // keep theirs until RESET KEYS on the CONTROLS page.
    for (const auto& key : lavender_shift::config::kDefaultKeys) {
      file << key.name << " = " << lavender_shift::config::Quote(key.keys) << "\n";
    }
#if defined(_WIN32)
    // LOW-SPEC's shader translation (src/ui/settings_menu.cpp, GRAPHICS
    // PRESET). It needs a restart, so it goes in before the GPU starts; a
    // capable machine sets it back for the next start (ApplyFirstRunDefaults).
    file << "spirv_specialize_texture_signs = true\n"
            "spirv_fast_pixel_math = true\n"
            "spirv_implicit_lod_2d = true\n";
#endif
  }
  host_config_ = std::make_unique<lavender_shift::config::HostConfig>(paths.config_path);
  // Mods that change more than how the game looks or sounds play a separate
  // profile, <user>-modded, which starts as a copy of the player's saves,
  // unlocks and caches. The settings, logs, photos, backups and save sync
  // stay with the player's own folder.
  enabled_mods_ = setting("enabled_mods", REXCVAR_GET(enabled_mods));
  if (!enabled_mods_.empty() &&
      !lavender_shift::mods::ShareSave(paths.user_data_root, enabled_mods_)) {
    const auto modded = lavender_shift::mods::ModdedProfile(paths.user_data_root);
    if (!std::filesystem::exists(modded, error)) {
      std::filesystem::create_directories(modded, error);
      for (const auto& entry : std::filesystem::directory_iterator(paths.user_data_root, error)) {
        const auto name = entry.path().filename();
        if (entry.is_directory(error) && (lavender_shift::IsSaveFolder(name) ||
                                          name == "achievements" || name == "cache")) {
          std::filesystem::copy(entry.path(), modded / name,
                                std::filesystem::copy_options::recursive, error);
        }
      }
      lavender_shift::diagnostics::RecordEvent(
          "mod.profile.created",
          {{"path", rex::path_to_utf8(modded)}, {"error", error ? error.message() : ""}});
    }
    paths.user_data_root = modded;
  }
}

// The enabled mods' files: game/ over the disc's files, textures/ and
// shaders/ ahead of any folders the settings name.
void LavenderShiftApp::MountMods() {
  // The immersive camera's camera.zip (src/mods/immersive_camera.h), kept in
  // the player's own cache and served behind every mod's files.
  const auto immersive_camera = lavender_shift::mods::BuildImmersiveCamera(
      game_data_root(), photos_dir_.parent_path() / "cache");
  std::vector<std::filesystem::path> game_roots;
  if (!enabled_mods_.empty()) lavender_shift::mods::Load(user_root_, enabled_mods_);
  const auto join = [](const std::vector<std::filesystem::path>& roots, std::string configured) {
    std::string dirs;
    for (const auto& root : roots) dirs += (dirs.empty() ? "" : ";") + rex::path_to_utf8(root);
    if (!configured.empty()) dirs += ";" + configured;
    return dirs;
  };
  // Defined in the GPU module, so set by name.
  for (const auto& [folder, cvar] : {std::pair{"textures", "texture_replacement_dirs"},
                                     std::pair{"shaders", "vulkan_shader_override_dirs"}}) {
    if (auto roots = lavender_shift::mods::Roots(folder); !roots.empty()) {
      const std::string dirs = join(roots, rex::cvar::GetFlagByName(cvar));
      rex::cvar::SetFlagByName(cvar, dirs);
      lavender_shift::diagnostics::RecordEvent("mod.folders", {{"kind", folder}, {"dirs", dirs}});
    }
  }
  if (!enabled_mods_.empty()) game_roots = lavender_shift::mods::Roots("game");
  if (!immersive_camera.empty()) game_roots.push_back(immersive_camera);
  // Mods' patches.toml edits (src/mods/data_patches.h), applied to the file
  // the game would read and served ahead of every mod's files.
  std::vector<std::filesystem::path> mod_directories;
  for (const auto& mod : lavender_shift::mods::Mods()) {
    if (mod.loaded) mod_directories.push_back(mod.directory);
  }
  if (!mod_directories.empty()) {
    const auto patched = photos_dir_.parent_path() / "cache" / "data-patches";
    const auto result = lavender_shift::mods::BuildDataPatches(mod_directories, game_data_root(),
                                                               game_roots, patched);
    for (const auto& problem : result.problems) REXLOG_WARN("Mods: {}", problem);
    if (result.files) {
      game_roots.insert(game_roots.begin(), patched);
      REXLOG_INFO("Mods: {} data patch(es) applied to {} file(s)", result.applied, result.files);
    }
    lavender_shift::diagnostics::RecordEvent(
        "mod.data_patches", {{"files", std::to_string(result.files)},
                             {"edits", std::to_string(result.applied)},
                             {"problems", std::to_string(result.problems.size())}});
  }
  // mods_log_game_files needs the overlay even with no mod files under it;
  // an empty folder of its own, never data-patches, whose files may be stale.
  if (game_roots.empty() && !rex::cvar::GetFlagByName("mods_log_game_files").empty()) {
    auto empty = photos_dir_.parent_path() / "cache" / "empty-overlay";
    std::error_code error;
    std::filesystem::create_directories(empty, error);
    game_roots.push_back(std::move(empty));
  }
  if (auto roots = std::move(game_roots); !roots.empty()) {
    auto overlay = std::make_unique<lavender_shift::mods::OverlayDevice>(
        "\\Device\\Harddisk0\\Partition1", game_data_root(), std::move(roots));
    if (overlay->Initialize() && runtime()->file_system()->ReplaceDevice(std::move(overlay))) {
      lavender_shift::diagnostics::RecordEvent("mod.overlay.mounted");
    } else {
      REXLOG_ERROR("Mods: could not mount the game file overlay");
      lavender_shift::diagnostics::RecordEvent("mod.overlay.error");
    }
  }
}

// --perf_log_csv=<path> writes one row of the SDK's counters per frame; the
// SDK leaves opening it to the app.
void LavenderShiftApp::OnPostInitLogging() {
  const std::string perf_csv = rex::cvar::GetFlagByName("perf_log_csv");
  if (!perf_csv.empty()) {
    rex::perf::SetCsvLogPath(perf_csv);
  }
}

namespace {

// Host keys. F-keys are media keys by default on Mac keyboards, so macOS
// adds its own conventions (Cmd+, settings, Ctrl+Cmd+F fullscreen, Cmd+Q
// quit), as Pinyon Shift does; the F-keys stay as alternatives.
#if defined(__APPLE__)
constexpr std::string_view kGameMenuKeys = "F6,Cmd+Comma";
constexpr std::string_view kPhotoKeys = "F8,Cmd+Shift+P";
constexpr std::string_view kHudKeys = "F2,Cmd+Shift+H";
constexpr std::string_view kFullscreenKeys = "F11,Ctrl+Cmd+F";
constexpr std::string_view kQuitKeys = "Cmd+Q";
#else
constexpr std::string_view kGameMenuKeys = "F6";
constexpr std::string_view kPhotoKeys = "F8";
constexpr std::string_view kHudKeys = "F2";
constexpr std::string_view kFullscreenKeys = "F11,Alt+Return";
constexpr std::string_view kQuitKeys = "";
#endif

}  // namespace

void LavenderShiftApp::OnPostSetup() {
#if !defined(__ANDROID__)
  rex::ui::RegisterBind("bind_frame_inspector", "Ctrl+F8", "Toggle the desktop frame inspector",
                        [this] { ToggleFrameInspector(); });
  if (REXCVAR_GET(fh1_frame_inspector) || REXCVAR_GET(fh1_frame_inspector_replay)) {
    ToggleFrameInspector();
  }
  // The pointer hides over the game window after a few idle seconds and
  // comes back when the mouse moves (Pinyon's #1076). Mouse capture hides
  // it and then restores this.
  if (window()) {
    window()->app_context().CallInUIThreadDeferred([this] {
      if (window()) window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    });
  }
#endif
  if (config_created_) ApplyFirstRunDefaults();
  MountMods();
  lavender_shift::movies::Install();
  lavender_shift::frame_rate::Install(window() ? window()->GetNativeWindowHandle() : nullptr);
  ApplyMasterVolume();
  rex::cvar::RegisterChangeCallback("lavender_master_volume",
                                    [](std::string_view, std::string_view) { ApplyMasterVolume(); });
  if (window() && !resize_listener_added_) {
    window()->AddListener(&resize_listener_);
    resize_listener_added_ = true;
  }
  // The game starts outside the gameplay view (wide_view starts narrow).
  rex::cvar::SetFlagByName("fh1_hud_scale_suspended", "true");
  lavender_shift::wide_view::SetNarrowCallback([this](bool narrow) {
    if (window()) {
      window()->app_context().CallInUIThreadDeferred([this, narrow] { ApplyNarrowView(narrow); });
    }
  });
  rex::cvar::RegisterChangeCallback("lavender_hor_plus", [this](std::string_view, std::string_view) {
    if (window()) {
      window()->app_context().CallInUIThreadDeferred([this] { ApplyNarrowView(narrow_view_); });
    }
  });
  UpdateHorPlus();
  lavender_shift::cvar_cycle::Start();
  // F6 here, and Back+Start on a pad (the SDK's pad_chord_game_menu).
  rex::ui::RegisterBind("bind_game_menu", kGameMenuKeys, "Open the in-game settings menu",
                        [this] { ToggleSettingsMenu(); });
  // The pause menu's SETTINGS tile (src/pause_menu_hooks.cpp) opens the same
  // menu; it runs on a guest thread, so post it to the UI thread.
  lavender_shift::pause_menu::SetSettingsHandler([this] {
    if (!window()) return;
    window()->app_context().CallInUIThreadDeferred([this] {
      if (!host_ui_ || !host_ui_->is_open()) ToggleSettingsMenu();
    });
  });
  rex::ui::RegisterBind("bind_photo", kPhotoKeys, "Save the current frame as a PNG photo", [this] {
    lavender_shift::ui::SavePhoto(
        runtime() && runtime()->graphics_system() ? runtime()->graphics_system()->presenter()
                                                  : nullptr,
        photos_dir_);
  });
  // LAVENDER_SHIFT_PHOTO_AT=<seconds>[,<seconds>...]: a photo, as the photo
  // key takes it, that many seconds after start. A check that needs no key
  // presses or screen-recording permission (a Mac driven over ssh).
  if (const char* times = std::getenv("LAVENDER_SHIFT_PHOTO_AT")) {
    std::vector<int> seconds;
    for (const char* at = times; *at;) {
      if (const int value = std::atoi(at); value > 0) seconds.push_back(value);
      at = std::strchr(at, ',');
      if (!at) break;
      ++at;
    }
    std::thread([this, seconds] {
      const auto start = std::chrono::steady_clock::now();
      for (const int at : seconds) {
        std::this_thread::sleep_until(start + std::chrono::seconds(at));
        if (!window()) return;
        window()->app_context().CallInUIThreadDeferred([this] {
          lavender_shift::ui::SavePhoto(runtime() && runtime()->graphics_system()
                                            ? runtime()->graphics_system()->presenter()
                                            : nullptr,
                                        photos_dir_);
        });
      }
    }).detach();
  }
  rex::ui::RegisterBind("bind_fullscreen", kFullscreenKeys, "Toggle fullscreen", [this] {
    const bool fullscreen = !REXCVAR_GET(fullscreen);
    rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
    if (host_config_ && host_config_->Load()) {
      host_config_->Set("fullscreen", fullscreen ? "true" : "false");
      host_config_->Save();
    }
  });
  rex::ui::RegisterBind("bind_hud", kHudKeys, "Hide or show the game's HUD",
                        [] { lavender_shift::graphics::ToggleHud(); });
  if (!kQuitKeys.empty()) {
    rex::ui::RegisterBind("bind_quit", kQuitKeys, "Quit the game", [this] { QuitFromMenu(); });
  }
  rex::ui::RegisterBind("bind_music_next", "F9", "Your music: next track",
                        [] { lavender_shift::music::Next(); });
  rex::ui::RegisterBind("bind_music_previous", "Shift+F9", "Your music: previous track",
                        [] { lavender_shift::music::Previous(); });
  lavender_shift::music::Install(user_root_, [this](std::function<void()> task) {
    if (window()) window()->app_context().CallInUIThreadDeferred(std::move(task));
  });
  // The Vulkan presenter picks its present mode from vsync when it creates
  // the swapchain, which a surface refresh does.
  // WINDOWS HDR on another MONITOR than the primary: through the window, then
  // the swapchain is made again so the presenter is offered scRGB.
  if (REXCVAR_GET(lavender_windows_hdr) && REXCVAR_GET(monitor) > 0 && window()) {
    TurnOnWindowsHdr(window()->GetNativeWindowHandle());
    RefreshSurface();
  }
  // WINDOWS HDR switched in the menu applies at once, the swapchain made again
  // for the display's new mode.
  rex::cvar::RegisterChangeCallback("lavender_windows_hdr",
                                    [this](std::string_view, std::string_view value) {
                                      if (!window()) return;
                                      const bool on = value == "true";
                                      window()->app_context().CallInUIThreadDeferred([this, on] {
                                        if (on) {
                                          TurnOnWindowsHdr(
                                              window() ? window()->GetNativeWindowHandle() : nullptr);
                                        } else {
                                          lavender_shift::platform::RestoreDisplayHdr();
                                        }
                                        RefreshSurface();
                                      });
                                    });
  rex::cvar::RegisterChangeCallback("vsync", [this](std::string_view, std::string_view) {
    if (window()) {
      window()->app_context().CallInUIThreadDeferred([this] { RefreshSurface(); });
    }
  });
  if (REXCVAR_GET(lavender_save_backups)) {
    save_backups_ = std::make_unique<lavender_shift::SaveBackups>(
        user_root_, user_root_ / "backups" / "saves",
        size_t(std::max(1, REXCVAR_GET(lavender_save_backup_count))));
    // Each snapshot is a copy of the saves taken while they were still, so
    // sync publishes from it, never from files the title may be writing.
    if (save_sync_) {
      save_backups_->SetOnSnapshot(
          [sync = save_sync_.get()](const std::filesystem::path& copy) { sync->Export(copy); });
    }
    save_backups_->Start();
  }
  if (save_sync_) {
    save_sync_->Watch([this](const std::vector<lavender_shift::SaveSync::Remote>& news,
                             bool first_poll) {
      if (first_poll) {
        if (sync_startup_.folder_unavailable) {
          PostToast("SAVE SYNC", "THE SYNC FOLDER IS NOT REACHABLE",
                    "This session's progress stays on this device");
          return;
        }
        if (!sync_startup_.imported_from.empty()) {
          PostToast("SAVE SYNC", "LOADED THE SAVE FROM " + sync_startup_.imported_from,
                    "This device's previous save is in SAVE BACKUPS");
        }
      }
      for (const auto& remote : news) {
        if (remote.relation == lavender_shift::SaveSync::Relation::kDiverged) {
          PostToast("SAVE SYNC", "YOUR SAVE ALSO CHANGED ON " + remote.device_name,
                    "Choose which to keep: SETTINGS > PROFILE > SAVE SYNC");
        } else {
          PostToast("SAVE SYNC", "A NEWER SAVE FROM " + remote.device_name + " IS WAITING",
                    "It loads when the game next starts");
        }
      }
    });
  }
  if (REXCVAR_GET(lavender_host_xam_dialogs)) {
    xam_dialogs_ = lavender_shift::ui::CreateXamDialogs(
        [this]() -> lavender_shift::hostui::HostUi* {
          return EnsureHostUi() ? host_ui_.get() : nullptr;
        });
    rex::kernel::xam::SetXamUiProvider(xam_dialogs_.get());
    // LAVENDER_SHIFT_XAM_DIALOG_SELF_TEST=<seconds>: a message box, then a
    // keyboard, as the game would open them; the answers go to the log.
    if (const char* delay = std::getenv("LAVENDER_SHIFT_XAM_DIALOG_SELF_TEST")) {
      const int seconds = std::max(1, std::atoi(delay));
      std::thread([this, seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        if (!window()) return;
        window()->app_context().CallInUIThreadDeferred([this] {
          xam_dialogs_->ShowMessageBox(
              "Self test", "A message box drawn by the host UI.", {"First", "Second"}, 0,
              [this](uint32_t button) {
                lavender_shift::diagnostics::RecordEvent(
                    "xam.dialog.self_test", {{"kind", "message_box"},
                                             {"button", std::to_string(int32_t(button))}});
                xam_dialogs_->ShowKeyboard(
                    "Self test", "Type a name", "LAV", 15,
                    [](bool accepted, std::string text) {
                      lavender_shift::diagnostics::RecordEvent(
                          "xam.dialog.self_test", {{"kind", "keyboard"},
                                                   {"accepted", accepted ? "1" : "0"},
                                                   {"text", text}});
                    });
              });
        });
      }).detach();
    }
  }
  // LAVENDER_SHIFT_SETTINGS_SELF_TEST=<seconds>: open the settings menu, as
  // F6 would, for scripted screenshots of its pages.
  if (const char* delay = std::getenv("LAVENDER_SHIFT_SETTINGS_SELF_TEST")) {
    const int seconds = std::max(1, std::atoi(delay));
    std::thread([this, seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      if (!window()) return;
      window()->app_context().CallInUIThreadDeferred([this] {
        if (!host_ui_ || !host_ui_->is_open()) ToggleSettingsMenu();
      });
    }).detach();
  }
  // The runtime's components may have replaced the crash filter by now.
  lavender_shift::diagnostics::RefreshCrashReporter();
}

void LavenderShiftApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_frame_inspector");
  frame_inspector_.reset();
  lavender_shift::guest_native::RecordSummary();
  rex::ui::UnregisterBind("bind_game_menu");
  lavender_shift::pause_menu::SetSettingsHandler(nullptr);
  lavender_shift::cvar_cycle::Stop();
  lavender_shift::frame_rate::Uninstall();
  rex::ui::UnregisterBind("bind_hud");
  rex::ui::UnregisterBind("bind_photo");
  rex::ui::UnregisterBind("bind_music_next");
  rex::ui::UnregisterBind("bind_music_previous");
  lavender_shift::music::Shutdown();
  rex::ui::UnregisterBind("bind_fullscreen");
  if (!kQuitKeys.empty()) rex::ui::UnregisterBind("bind_quit");
  lavender_shift::movies::Uninstall();
  lavender_shift::wide_view::SetNarrowCallback(nullptr);
  rex::cvar::UnregisterChangeCallbacks("lavender_hor_plus");
  rex::cvar::UnregisterChangeCallbacks("lavender_master_volume");
  if (resize_listener_added_ && window()) {
    window()->RemoveListener(&resize_listener_);
    resize_listener_added_ = false;
  }
  rex::cvar::UnregisterChangeCallbacks("vsync");
  rex::cvar::UnregisterChangeCallbacks("lavender_windows_hdr");
  // Before the presenter and drawer it uses are torn down.
  rex::kernel::xam::SetXamUiProvider(nullptr);
  xam_dialogs_.reset();
  host_ui_.reset();
  // Takes the exit backup when the saves changed since the last one.
  save_backups_.reset();
  // The title has stopped: publish the save as it was left (also when
  // backups are off, or a SAVE SYNC choice changed what to publish).
  if (save_sync_) {
    save_sync_->StopWatching();
    save_sync_->Export(user_root_);
  }
  lavender_shift::platform::RestoreDisplayHdr();
}

void LavenderShiftApp::PostToast(std::string heading, std::string title, std::string detail) {
  if (!window()) return;
  window()->app_context().CallInUIThreadDeferred(
      [this, heading = std::move(heading), title = std::move(title),
       detail = std::move(detail)]() mutable {
        if (EnsureHostUi()) {
          host_ui_->ShowToast(std::move(heading), std::move(title), std::move(detail), nullptr);
        }
      });
}

void LavenderShiftApp::ToggleFrameInspector() {
#if !defined(__ANDROID__)
  if (frame_inspector_ && frame_inspector_->is_open()) {
    frame_inspector_.reset();
    return;
  }
  auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
  if (!graphics || !imgui_drawer() || !immediate_drawer()) return;
  frame_inspector_ = std::make_unique<lavender_shift::ui::FrameInspector>(
      *imgui_drawer(), *immediate_drawer(), *graphics, REXCVAR_GET(fh1_frame_inspector_replay));
#endif
}

// A frame replay (fh1_frame_replay) or the replay workspace drives the GPU on
// its own; the title stays suspended.
bool LavenderShiftApp::ShouldStartModuleThread() {
#if !defined(__ANDROID__)
  if (REXCVAR_GET(fh1_frame_inspector_replay)) return false;
#endif
  return rex::cvar::GetFlagByName("fh1_frame_replay").empty();
}

void LavenderShiftApp::RefreshSurface() {
  auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
  if (graphics && graphics->presenter()) {
    graphics->presenter()->OnSurfaceResizeFromUIThread();
  }
}

bool LavenderShiftApp::EnsureHostUi() {
  if (host_ui_) {
    return true;
  }
  auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
  rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
  if (!presenter || !immediate_drawer() || !window()) {
    REXLOG_WARN("Host UI: presentation is not ready");
    return false;
  }
  host_ui_ = std::make_unique<lavender_shift::hostui::HostUi>(
      *this, *presenter, *immediate_drawer(), *window(),
      static_cast<rex::input::InputSystem*>(runtime()->input_system()), game_data_root());
  return true;
}

void LavenderShiftApp::ToggleSettingsMenu() {
  if (host_ui_ && host_ui_->is_open()) {
    host_ui_->Close();
    return;
  }
  if (!EnsureHostUi() || !host_config_) {
    return;
  }
  lavender_shift::ui::SettingsServices services;
  services.quit = [this] { QuitFromMenu(); };
  services.save_backups = save_backups_.get();
  services.save_sync = save_sync_.get();
  if (!user_root_.empty()) services.luts_dir = user_root_ / "luts";
  if (!user_root_.empty()) services.texture_dumps_dir = user_root_ / "texture-dumps";
  services.draw_resolution_scale = [this]() -> uint32_t {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    return graphics ? graphics->draw_resolution_scale() : 0;
  };
  services.output_size = [this]() -> std::optional<std::pair<uint32_t, uint32_t>> {
    const uint32_t width = window() ? window()->GetActualPhysicalWidth() : 0;
    const uint32_t height = window() ? window()->GetActualPhysicalHeight() : 0;
    if (!width || !height) {
      return std::nullopt;
    }
    if (rex::cvar::GetFlagByName("present_letterbox") != "true") {
      return std::pair{width, height};
    }
    return uint64_t(width) * 9 > uint64_t(height) * 16 ? std::pair{height * 16 / 9, height}
                                                       : std::pair{width, width * 9 / 16};
  };
  services.upscalers = [this]() -> std::vector<rex::ui::UpscalerInfo> {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter ? presenter->GetUpscalers() : rex::ui::GetSpatialUpscalerInfos();
  };
  services.frame_generators = [this]() -> std::vector<rex::ui::UpscalerInfo> {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter ? presenter->GetFrameGenerators() : std::vector<rex::ui::UpscalerInfo>{};
  };
  host_ui_->Open(lavender_shift::ui::CreateSettingsMenu(*host_ui_, *host_config_, services));
}

void LavenderShiftApp::QuitFromMenu() {
  // A programmatic close skips the close-request handling; run it first, as
  // the close button does.
  app_context().CallInUIThread([this] {
    if (window() && OnWindowCloseRequested()) window()->RequestClose();
  });
}

void LavenderShiftApp::UpdateHorPlus() {
  float scale = 1.0f;
  if (REXCVAR_GET(lavender_hor_plus) && window()) {
    const uint32_t width = window()->GetActualPhysicalWidth();
    const uint32_t height = window()->GetActualPhysicalHeight();
    if (width && height) {
      scale = std::max(1.0f, (float(width) / float(height)) / (16.0f / 9.0f));
    }
  }
  lavender_shift::wide_view::SetAspectScale(scale);
  // The HUD keeps 16:9 proportions in the stretched image (the SDK squeezes
  // draws into the front buffer's 2_10_10_10 view).
  rex::cvar::SetFlagByName("fh1_hud_squeeze", narrow_view_ ? "1.0" : std::to_string(scale));
}

void LavenderShiftApp::ApplyNarrowView(bool narrow) {
  narrow_view_ = narrow;
  // HUD SCALE and SAFE ZONE move the gameplay HUD only; the game's own
  // screens keep their layout, as in Pinyon Shift's map view.
  rex::cvar::SetFlagByName("fh1_hud_scale_suspended", narrow ? "true" : "false");
  // The gameplay view is stretched over the window; the game's own 16:9
  // screens (menus, loading, the map) are letterboxed. Without WIDER VIEW the
  // ASPECT RATIO setting owns the letterbox.
  if (REXCVAR_GET(lavender_hor_plus)) {
    rex::cvar::SetFlagByName("present_letterbox", narrow ? "true" : "false");
  }
  UpdateHorPlus();
}

// A new settings file starts at PERFORMANCE 60 (the SDK's defaults) on a
// discrete GPU with 4 GB or more of its own memory and 6 or more logical
// cores, and at LOW-SPEC 60 otherwise: the cheapest look at the full rate.
// Only on the launch that created the file, so a player's own choices are
// never changed. Pinyon Shift's LS-1.5 rule, with FH2's top rate of 60;
// the thresholds are not yet measured on FH2's low end.
void LavenderShiftApp::ApplyFirstRunDefaults() {
#if defined(__ANDROID__)
  // Phones and handhelds: 1x with 2x MSAA, reflections updated every
  // fourth frame and no sun shadows, which holds 60 fps on a Snapdragon
  // 8 Gen 2 with the GPU about 76 % busy (docs/ANDROID.md, Results).
  static constexpr std::pair<const char*, const char*> kAndroid[] = {
      {"draw_resolution_scale_x", "1"},     {"draw_resolution_scale_y", "1"},
      {"present_effect", "\"bilinear\""},   {"fh1_msaa_single_sample", "false"},
      {"fh1_msaa_2x", "true"},              {"lavender_env_map_rate", "0.25"},
      {"lavender_shadows", "false"},        {"lavender_frame_rate", "\"60\""}};
  if (!host_config_ || !host_config_->Load()) return;
  for (const auto& [name, literal] : kAndroid) {
    host_config_->Set(name, literal);
    std::string value = literal;
    std::erase(value, '"');
    rex::cvar::SetFlagByName(name, value);
  }
  if (!host_config_->Save()) {
    REXLOG_ERROR("Settings: could not write {}", host_config_->path().string());
  }
  REXLOG_INFO("First run on Android: 1x, 2x MSAA, quarter reflections, no shadows, 60 fps");
  return;
#endif
  auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
  auto* provider = graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider())
                            : nullptr;
  auto* device = provider ? provider->vulkan_device() : nullptr;
  if (!device || !host_config_) return;
  const auto& functions = device->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties = {};
  functions.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
  VkPhysicalDeviceMemoryProperties memory = {};
  functions.vkGetPhysicalDeviceMemoryProperties(device->physical_device(), &memory);
  uint64_t device_local = 0;
  for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      device_local = std::max(device_local, uint64_t(memory.memoryHeaps[i].size));
    }
  }
  const bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  const uint32_t cores = rex::thread::logical_processor_count();
  const bool capable = discrete && device_local >= (uint64_t(4) << 30) && cores >= 6;
  REXLOG_INFO("First run on {} ({} MB, {} cores): {}", properties.deviceName, device_local >> 20,
              cores, capable ? "PERFORMANCE" : "LOW-SPEC, 60 fps");
  if (capable) {
#if defined(_WIN32)
    // PERFORMANCE's shader translation, from the next start: the translator
    // has read the new file's LOW-SPEC values by now.
    if (host_config_->Load()) {
      for (const char* name :
           {"spirv_specialize_texture_signs", "spirv_fast_pixel_math", "spirv_implicit_lod_2d"}) {
        host_config_->Set(name, "false");
      }
      if (!host_config_->Save()) {
        REXLOG_ERROR("Settings: could not write {}", host_config_->path().string());
      }
    }
#endif
    return;
  }
  // The GRAPHICS PRESET row's LOW-SPEC (src/ui/settings_menu.cpp), at 60 fps.
  static constexpr std::pair<const char*, const char*> kLowSpec[] = {
      {"present_effect", "\"bilinear\""}, {"fh1_msaa_single_sample", "true"},
      {"anisotropic_override", "-1"},     {"swap_post_effect", "\"none\""},
      {"lavender_frame_rate", "\"60\""}};
  if (!host_config_->Load()) return;
  for (const auto& [name, literal] : kLowSpec) {
    host_config_->Set(name, literal);
    std::string value = literal;
    std::erase(value, '"');
    rex::cvar::SetFlagByName(name, value);
  }
  if (!host_config_->Save()) {
    REXLOG_ERROR("Settings: could not write {}", host_config_->path().string());
  }
}
