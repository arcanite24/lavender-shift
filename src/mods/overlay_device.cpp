#include "mods/overlay_device.h"

#include <mutex>
#include <set>

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>
#include <rex/filesystem.h>

#include "diagnostics.h"

REXCVAR_DEFINE_STRING(mods_log_game_files, "", "Mods",
                      "Log each game file the title opens whose path contains this text, once "
                      "(for example .xml, or * for every file); empty: off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace lavender_shift::mods {

OverlayDevice::OverlayDevice(std::string_view mount_path, const std::filesystem::path& base_root,
                             std::vector<std::filesystem::path> overlay_roots)
    : Device(mount_path) {
  // Game files are read-only, the base and every overlay alike.
  base_ = std::make_unique<rex::filesystem::HostPathDevice>(mount_path, base_root, true);
  for (const auto& root : overlay_roots) {
    overlays_.push_back(
        std::make_unique<rex::filesystem::HostPathDevice>(mount_path, root, true));
  }
}

bool OverlayDevice::Initialize() {
  if (!base_->Initialize()) {
    return false;
  }
  for (auto it = overlays_.begin(); it != overlays_.end();) {
    if ((*it)->Initialize()) {
      ++it;
    } else {
      REXLOG_WARN("Mods: cannot read {}; its files are not used", rex::path_to_utf8((*it)->host_path()));
      it = overlays_.erase(it);
    }
  }
  // Files a mod adds to the game's directories are listed with them, as if
  // they were on the disc: the title enumerates directories (FilesToCache.xml
  // names wildcards such as media\Audio\Cars\Engines\Soundbanks\Burbles\*.*),
  // and a mod can add files beside the disc's. Earlier mods
  // first, as they win when opening.
  size_t listed = 0;
  for (const auto& overlay : overlays_) {
    listed += base_->MergeTree(overlay->host_path());
  }
  if (listed) {
    diagnostics::RecordEvent("mod.overlay.listed", {{"entries", std::to_string(listed)}});
  }
  return true;
}

void OverlayDevice::Dump(rex::string::StringBuffer* string_buffer) {
  base_->Dump(string_buffer);
}

namespace {

std::mutex g_overridden_mutex;
std::set<std::string, std::less<>> g_overridden;

// "\media\db\gamedb.slt" for "game:\media\DB\gamedb.slt" or "media/db/gamedb.slt".
std::string NormalizeGamePath(std::string_view path) {
  if (const size_t colon = path.find(':'); colon != std::string_view::npos) {
    path.remove_prefix(colon + 1);
  }
  std::string normal;
  normal.reserve(path.size() + 1);
  for (const char c : path) {
    const char slash = c == '/' ? '\\' : c;
    if (slash == '\\' && !normal.empty() && normal.back() == '\\') continue;
    normal.push_back(slash >= 'A' && slash <= 'Z' ? char(slash - 'A' + 'a') : slash);
  }
  if (normal.empty() || normal.front() != '\\') normal.insert(normal.begin(), '\\');
  return normal;
}

}  // namespace

bool IsOverriddenGamePath(std::string_view guest_path) {
  // The title's hash table names files relative to a root ("db\gamedb.slt"
  // under media), so a replaced path matches when it ends with this one at a
  // separator.
  const std::string normal = NormalizeGamePath(guest_path);
  std::lock_guard lock(g_overridden_mutex);
  for (const auto& overridden : g_overridden) {
    if (overridden.size() >= normal.size() &&
        overridden.compare(overridden.size() - normal.size(), normal.size(), normal) == 0) {
      return true;
    }
  }
  return false;
}

namespace {

std::mutex g_logged_mutex;
std::set<std::string, std::less<>> g_logged;

// mods_log_game_files: which game files the title reads, to see whether a
// data patch's file is used at all (issue #5 SR-16).
void LogGameFile(std::string_view path) {
  static const std::string filter = [] {
    std::string text = REXCVAR_GET(mods_log_game_files);
    for (char& c : text) c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    return text;
  }();
  if (filter.empty()) return;
  const std::string normal = NormalizeGamePath(path);
  if (filter != "*" && normal.find(filter) == std::string::npos) return;
  {
    std::lock_guard lock(g_logged_mutex);
    if (!g_logged.insert(normal).second) return;
  }
  REXLOG_INFO("Mods: the game opened {}", path);
  diagnostics::RecordEvent("mod.file.read", {{"path", std::string(path)}});
}

}  // namespace

rex::filesystem::Entry* OverlayDevice::ResolvePath(std::string_view path) {
  if (!path.empty()) LogGameFile(path);
  if (!path.empty()) {
    for (const auto& overlay : overlays_) {
      rex::filesystem::Entry* entry = overlay->ResolvePath(path);
      // Only files replace; a mod's directories must not hide the game's.
      if (entry && !(entry->attributes() & rex::filesystem::kFileAttributeDirectory)) {
        diagnostics::RecordEvent("mod.file.override",
                                 {{"path", std::string(path)},
                                  {"from", rex::path_to_utf8(overlay->host_path())}});
        {
          std::lock_guard lock(g_overridden_mutex);
          g_overridden.insert(NormalizeGamePath(path));
        }
        return entry;
      }
    }
  }
  if (auto* entry = base_->ResolvePath(path)) return entry;
  // New assets may introduce directories absent from the disc. VFS file
  // opens resolve the parent first; keep existing base directories intact,
  // but expose an overlay directory when it has no base counterpart.
  for (const auto& overlay : overlays_) {
    if (auto* entry = overlay->ResolvePath(path)) return entry;
  }
  return nullptr;
}

}  // namespace lavender_shift::mods
