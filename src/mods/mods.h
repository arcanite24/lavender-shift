#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace lavender_shift::mods {

// Mods (docs/PINYON_PARITY.md LP-1.12), Pinyon Shift's asset mods: each is a
// folder <user>/mods/<name>/ with a mod.toml and any of
//
//   game/      files that replace the disc's with the same path (or add new
//              ones), through an overlay device over the game drive
//   textures/  texture replacements (<hash>.dds, the SDK's
//              texture_replacement_dirs)
//   shaders/   shader overrides (the SDK's vulkan_shader_override_dirs)
//
// mod.toml is flat: name (the folder's name), version, shares_save (true for
// a mod that changes only how the game looks or sounds), and optional lists
// requires, load_after and conflicts. Pinyon's native mods (a library with
// its mod ABI) are built on FH1's symbol tables and are not carried; a mod
// that names a library is rejected. Mods are chosen with enabled_mods (a
// comma-separated list, earlier wins) and take effect at the next start.

struct ModInfo {
  std::string name;
  std::string version;
  std::filesystem::path directory;
  bool loaded = false;
  std::string problem;  // why it was not loaded
};

// Reads and orders the mods `enabled_mods` names under `user_root`/mods:
// after the mods each requires or loads after, otherwise in list order.
void Load(const std::filesystem::path& user_root, const std::string& enabled_mods);
const std::vector<ModInfo>& Mods();

// True when every enabled mod declares shares_save, so the player's own saves
// stay in use; otherwise the game plays the modded profile. Read before the
// mods load, while the paths are chosen.
bool ShareSave(const std::filesystem::path& user_root, const std::string& enabled_mods);
// The separate save folder for mods that do not share the save:
// <user>-modded beside the user folder.
std::filesystem::path ModdedProfile(const std::filesystem::path& user_root);

// The loaded mods' game/, textures/ and shaders/ folders, in priority order.
std::vector<std::filesystem::path> Roots(std::string_view directory);

}  // namespace lavender_shift::mods
