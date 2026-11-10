#pragma once

#include <string_view>

namespace lavender_shift::config {

struct KeyDefault {
  std::string_view name;
  std::string_view keys;
};

// Keyboard defaults chosen for driving, Pinyon Shift's set (its #432): the
// pad's Forza roles are RT throttle, LT brake, the left stick steers and
// moves through menus, A handbrake and accept, B shift up and back, X shift
// down, Y rewind, Start pause. Driving needs only the keys around WASD, and
// nothing needs a numpad. New settings files get all of them; the CONTROLS
// page's RESET KEYS restores them. Files made before this keep their keys.
inline constexpr KeyDefault kDefaultKeys[] = {
    {"keybind_a", "Space,Return,LMB"},
    {"keybind_b", "E,Backspace"},
    {"keybind_x", "Q"},
    {"keybind_y", "R"},
    {"keybind_left_trigger", "S"},
    {"keybind_right_trigger", "W"},
    {"keybind_left_shoulder", "Z"},
    {"keybind_right_shoulder", "C"},
    {"keybind_lstick_up", "Up"},
    {"keybind_lstick_down", "Down"},
    {"keybind_lstick_left", "A,Left"},
    {"keybind_lstick_right", "D,Right"},
    {"keybind_lstick_press", "F"},
    {"keybind_rstick_up", "I"},
    {"keybind_rstick_down", "K"},
    {"keybind_rstick_left", "J"},
    {"keybind_rstick_right", "L"},
    {"keybind_rstick_press", "V"},
    {"keybind_dpad_up", "Shift+Up"},
    {"keybind_dpad_down", "Shift+Down"},
    {"keybind_dpad_left", "Shift+Left"},
    {"keybind_dpad_right", "Shift+Right"},
    {"keybind_back", "Tab,M"},
    {"keybind_start", "Escape"},
};

}  // namespace lavender_shift::config
