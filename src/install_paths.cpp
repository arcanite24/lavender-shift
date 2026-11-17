#include "install_paths.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>

namespace lavender_shift::install {
namespace {

namespace fs = std::filesystem;

bool IsDefault(const char* name) {
  return rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault;
}

}  // namespace

bool ResolveDefaults(rex::PathConfig& paths) {
  if (!paths.game_data_root.empty() || !IsDefault("game_data_root")) return false;
  std::error_code error;
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  const fs::path source = exe_dir.parent_path().parent_path().parent_path();
  const fs::path game = source / ".local" / "game";
  if (!fs::is_regular_file(game / "disc" / "default.xex", error)) return false;

  paths.game_data_root = game / "disc";
  if (paths.update_data_root.empty() && IsDefault("update_data_root") &&
      fs::is_directory(game / "update", error)) {
    paths.update_data_root = game / "update";
  }
  if (IsDefault("user_data_root")) {
    // The launcher's layout (launcher/.../InstallLayout.cs): a release keeps
    // the player's folder beside the source folder, a checkout in .local.
    const fs::path install = source.parent_path();
    paths.user_data_root = fs::is_regular_file(install / "lavender-shift-source.zip", error)
                               ? install / "user"
                               : source / ".local" / "user";
    fs::create_directories(paths.user_data_root, error);
    if (IsDefault("cache_root")) paths.cache_root = paths.user_data_root / "cache";
  }
  // tools/launch.ps1's flags.
  for (const auto& [name, value] :
       {std::pair{"xex_apply_patches", "true"}, std::pair{"gpu_backend", "vulkan"},
        std::pair{"xam_xstudio_request_unsupported", "true"},
        std::pair{"gpu_read_pointer_publish_dwords", "1024"}}) {
    if (IsDefault(name)) rex::cvar::SetFlagByName(name, value);
  }
  return true;
}

bool ClaimUserRoot(const fs::path& user_root) {
#if defined(_WIN32)
  std::error_code error;
  std::wstring key = fs::weakly_canonical(user_root, error).wstring();
  if (key.empty()) key = user_root.wstring();
  std::transform(key.begin(), key.end(), key.begin(),
                 [](wchar_t c) { return wchar_t(std::towlower(c)); });
  const std::wstring name =
      L"Local\\LavenderShift-" + std::to_wstring(std::hash<std::wstring>{}(key));
  // Held until the process exits.
  const HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
  return mutex && GetLastError() != ERROR_ALREADY_EXISTS;
#else
  (void)user_root;
  return true;
#endif
}

void ShowAlreadyRunning() {
#if defined(_WIN32)
  MessageBoxW(nullptr, L"Lavender Shift is already running.", L"Lavender Shift",
              MB_OK | MB_ICONINFORMATION);
#endif
}

}  // namespace lavender_shift::install
