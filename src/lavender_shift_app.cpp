#include "lavender_shift_app.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
#include "graphics_hooks.h"
#include "movie_hooks.h"
#include "wide_view.h"
#include "ui/photo_export.h"
#include "cvar_cycle.h"
#include "diagnostics.h"
#include "save_backups.h"
#include "pause_menu_hooks.h"
#include "ui/settings_menu.h"
#include "ui/xam_dialogs.h"

LavenderShiftApp::~LavenderShiftApp() = default;

REXCVAR_DECLARE(std::string, gpu_backend);
REXCVAR_DECLARE(bool, fullscreen);

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
REXCVAR_DEFINE_BOOL(lavender_hor_plus, false, "Display",
                    "Ultrawide: widen the horizontal field of view to the window's aspect "
                    "(Hor+), shown stretched from the 16:9 image; the HUD keeps its shape")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Stage 0 renders through ShiftGlue's Xenos (PM4) plugin, rexgpu-fh1, unless
// --gpu_plugin names another one.
void LavenderShiftApp::OnPreSetup(rex::RuntimeConfig& config) {
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
  // Crash reports, stall dumps and session events (LP-1.10) from here on.
  lavender_shift::diagnostics::Initialize(paths.user_data_root);
  paths.config_path = paths.user_data_root / "config" / "lavender_shift.toml";
  // A restore chosen on the PROFILE page, before the title reads its saves.
  lavender_shift::SaveBackups::ApplyPendingRestore(paths.user_data_root,
                                                   paths.user_data_root / "backups" / "saves");
  user_root_ = paths.user_data_root;
  photos_dir_ = paths.user_data_root / "photos";
  std::error_code error;
  std::filesystem::create_directories(paths.config_path.parent_path(), error);
  if (!std::filesystem::exists(paths.config_path, error)) {
    config_created_ = true;
    std::ofstream file(paths.config_path);
    file << "# Lavender Shift settings, written by the in-game menu (F6).\n";
    // Driving-first keyboard keys (src/config/default_keys.h); older files
    // keep theirs until RESET KEYS on the CONTROLS page.
    for (const auto& key : lavender_shift::config::kDefaultKeys) {
      file << key.name << " = " << lavender_shift::config::Quote(key.keys) << "\n";
    }
  }
  host_config_ = std::make_unique<lavender_shift::config::HostConfig>(paths.config_path);
}

// --perf_log_csv=<path> writes one row of the SDK's counters per frame; the
// SDK leaves opening it to the app.
void LavenderShiftApp::OnPostInitLogging() {
  const std::string perf_csv = rex::cvar::GetFlagByName("perf_log_csv");
  if (!perf_csv.empty()) {
    rex::perf::SetCsvLogPath(perf_csv);
  }
}

void LavenderShiftApp::OnPostSetup() {
  if (config_created_) ApplyFirstRunDefaults();
  lavender_shift::movies::Install();
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
  rex::ui::RegisterBind("bind_game_menu", "F6", "Open the in-game settings menu",
                        [this] { ToggleSettingsMenu(); });
  // The pause menu's SETTINGS tile (src/pause_menu_hooks.cpp) opens the same
  // menu; it runs on a guest thread, so post it to the UI thread.
  lavender_shift::pause_menu::SetSettingsHandler([this] {
    if (!window()) return;
    window()->app_context().CallInUIThreadDeferred([this] {
      if (!host_ui_ || !host_ui_->is_open()) ToggleSettingsMenu();
    });
  });
  rex::ui::RegisterBind("bind_photo", "F8", "Save the current frame as a PNG photo", [this] {
    lavender_shift::ui::SavePhoto(
        runtime() && runtime()->graphics_system() ? runtime()->graphics_system()->presenter()
                                                  : nullptr,
        photos_dir_);
  });
  rex::ui::RegisterBind("bind_fullscreen", "F11,Alt+Return", "Toggle fullscreen", [this] {
    const bool fullscreen = !REXCVAR_GET(fullscreen);
    rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
    if (host_config_ && host_config_->Load()) {
      host_config_->Set("fullscreen", fullscreen ? "true" : "false");
      host_config_->Save();
    }
  });
  rex::ui::RegisterBind("bind_hud", "F2", "Hide or show the game's HUD",
                        [] { lavender_shift::graphics::ToggleHud(); });
  // The Vulkan presenter picks its present mode from vsync when it creates
  // the swapchain, which a surface refresh does.
  rex::cvar::RegisterChangeCallback("vsync", [this](std::string_view, std::string_view) {
    if (window()) {
      window()->app_context().CallInUIThreadDeferred([this] { RefreshSurface(); });
    }
  });
  if (REXCVAR_GET(lavender_save_backups)) {
    save_backups_ = std::make_unique<lavender_shift::SaveBackups>(
        user_root_, user_root_ / "backups" / "saves",
        size_t(std::max(1, REXCVAR_GET(lavender_save_backup_count))));
    save_backups_->Start();
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
  // The runtime's components may have replaced the crash filter by now.
  lavender_shift::diagnostics::RefreshCrashReporter();
}

void LavenderShiftApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_game_menu");
  lavender_shift::pause_menu::SetSettingsHandler(nullptr);
  lavender_shift::cvar_cycle::Stop();
  rex::ui::UnregisterBind("bind_hud");
  rex::ui::UnregisterBind("bind_photo");
  rex::ui::UnregisterBind("bind_fullscreen");
  lavender_shift::movies::Uninstall();
  lavender_shift::wide_view::SetNarrowCallback(nullptr);
  rex::cvar::UnregisterChangeCallbacks("lavender_hor_plus");
  rex::cvar::UnregisterChangeCallbacks("lavender_master_volume");
  if (resize_listener_added_ && window()) {
    window()->RemoveListener(&resize_listener_);
    resize_listener_added_ = false;
  }
  rex::cvar::UnregisterChangeCallbacks("vsync");
  // Before the presenter and drawer it uses are torn down.
  rex::kernel::xam::SetXamUiProvider(nullptr);
  xam_dialogs_.reset();
  host_ui_.reset();
  // Takes the exit backup when the saves changed since the last one.
  save_backups_.reset();
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
              cores, capable ? "PERFORMANCE 60" : "LOW-SPEC 60");
  if (capable) return;
  // The GRAPHICS PRESET row's LOW-SPEC 60 (src/ui/settings_menu.cpp).
  static constexpr std::pair<const char*, const char*> kLowSpec[] = {
      {"present_effect", "\"bilinear\""}, {"fh1_msaa_single_sample", "true"},
      {"anisotropic_override", "-1"},     {"swap_post_effect", "\"none\""}};
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
