#pragma once

#include <functional>
#include <memory>

#include <rex/kernel/xam/ui_provider.h>

namespace lavender_shift::hostui {
class HostUi;
class MenuScreen;
}

namespace lavender_shift::ui {

// The XAM message box and keyboard as host UI screens, Pinyon Shift's
// (docs/PINYON_PARITY.md LP-1.9): the title's fonts and layout, pad,
// keyboard and mouse input, B or Escape to cancel. `host_ui` returns the
// host UI, made on first use (null when presentation is not ready);
// `achievements` builds the list XamShowAchievementsUI opens (none: closes).
std::unique_ptr<rex::kernel::xam::XamUiProvider> CreateXamDialogs(
    std::function<hostui::HostUi*()> host_ui,
    std::function<std::unique_ptr<hostui::MenuScreen>()> achievements = {});

}  // namespace lavender_shift::ui
