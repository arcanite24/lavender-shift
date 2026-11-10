// Lavender Shift - Forza Horizon 2 on ShiftGlue.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/ui/window_listener.h>

#include <rex/image_info.h>
#include <rex/rex_app.h>

#include "config/host_config.h"
#include "save_backups.h"
#include <rex/kernel/xam/ui_provider.h>
#include "ui/hostui/host_ui.h"

// From the generated init header (lavender_shift_pch.h).
extern const rex::PPCImageInfo PPCImageConfig;

class LavenderShiftApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~LavenderShiftApp() override;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LavenderShiftApp>(
        new LavenderShiftApp(ctx, "lavender_shift", PPCImageConfig));
  }

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostInitLogging() override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  // The in-game settings menu (F6, or Back+Start on a pad).
  void ToggleSettingsMenu();
  bool EnsureHostUi();
  void QuitFromMenu();
  // Recreates the swapchain, for settings it reads when it is created.
  void RefreshSurface();
  // WIDER VIEW (Hor+): the widening for the window's aspect, and the switch
  // between the stretched gameplay view and the game's letterboxed screens.
  void UpdateHorPlus();
  void ApplyNarrowView(bool narrow);
  // The preset for this machine, on the launch that created the settings file.
  void ApplyFirstRunDefaults();
  bool config_created_ = false;
  std::filesystem::path photos_dir_;

  class ResizeListener final : public rex::ui::WindowListener {
   public:
    explicit ResizeListener(LavenderShiftApp& app) : app_(app) {}
    void OnResize(rex::ui::UISetupEvent&) override { app_.UpdateHorPlus(); }

   private:
    LavenderShiftApp& app_;
  };
  ResizeListener resize_listener_{*this};
  bool resize_listener_added_ = false;
  bool narrow_view_ = true;

  std::unique_ptr<lavender_shift::config::HostConfig> host_config_;
  std::unique_ptr<lavender_shift::hostui::HostUi> host_ui_;
  std::unique_ptr<lavender_shift::SaveBackups> save_backups_;
  std::unique_ptr<rex::kernel::xam::XamUiProvider> xam_dialogs_;
  std::filesystem::path user_root_;
};
