#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <rex/ui/upscaler.h>

#include "config/host_config.h"
#include "ui/hostui/host_ui.h"
#include "ui/hostui/menu.h"

namespace lavender_shift {
class SaveBackups;
}

namespace lavender_shift::ui {

// What the settings screen reads from the running game.
struct SettingsServices {
  // The draw resolution scale the renderer uses now (0 when unknown).
  std::function<uint32_t()> draw_resolution_scale;
  // Where the latest frame reached the window, in pixels.
  std::function<std::optional<std::pair<uint32_t, uint32_t>>()> output_size;
  // The output scalers the presenter has; temporal ones say whether their
  // runtime loaded.
  std::function<std::vector<rex::ui::UpscalerInfo>()> upscalers;
  // Closes the window the way its close button does; no row without it.
  std::function<void()> quit;
  // The save backups; no SAVE BACKUPS row without them.
  SaveBackups* save_backups = nullptr;
};

// The in-game settings screen: DISPLAY and GRAPHICS pages over the host UI.
// Every row applies while the game runs (the project's rule: no setting
// waits for a restart) and is saved to `config` at once, with a backup
// before the first save of the session.
std::unique_ptr<hostui::MenuScreen> CreateSettingsMenu(hostui::HostUi& host_ui,
                                                       config::HostConfig& config,
                                                       SettingsServices services = {});

}  // namespace lavender_shift::ui
