#pragma once

#include <filesystem>

namespace rex::ui {
class Presenter;
}

namespace lavender_shift::ui {
// Saves the current game frame (without host overlays) as a PNG in
// `directory`, on a worker thread; one photo at a time.
void SavePhoto(rex::ui::Presenter* presenter, std::filesystem::path directory);
}  // namespace lavender_shift::ui
