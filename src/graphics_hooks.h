#pragma once

namespace lavender_shift::graphics {

// Flips hud_hidden (the F2 bind).
void ToggleHud();
// Applies hud_hidden; called once per simulation step on the game thread.
void UpdateHud();
// Applies disable_motion_blur to the blur strengths the scene stored.
void ApplyMotionBlur();
// Logs the settings hooks' call counts every 600 steps (lavender_log_hooks).
void LogHookCalls();

}  // namespace lavender_shift::graphics
