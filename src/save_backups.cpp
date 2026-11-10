#include "save_backups.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/filesystem.h>

#include "diagnostics.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lavender_shift {
namespace fs = std::filesystem;
namespace {

constexpr auto kPollInterval = std::chrono::seconds(5);
// The title autosaves after most events, often 15 to 30 s apart, and each
// backup copies the whole save tree (about 24 MB on a finished save).
// Copying it after every save showed up as disk-use spikes and hitches, so
// backups are at most this frequent during play, with one more at exit.
constexpr auto kMinSnapshotInterval = std::chrono::minutes(10);
constexpr const char* kRestoreFile = "restore-pending.txt";

// A profile's folder: 16 hex digits, not the all-zero folder that holds
// installed content.
bool IsSaveFolder(const fs::path& name) {
  const std::string text = rex::path_to_utf8(name);
  return text.size() == 16 && text != "0000000000000000" &&
         std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(uint8_t(c)); });
}

// Outside the saves: an entry of the user folder that is not a profile.
bool IsOtherPath(const fs::path& relative) {
  return relative.begin() != relative.end() && !IsSaveFolder(*relative.begin());
}

std::string UtcStamp() {
  const auto now = std::chrono::system_clock::now();
  return fmt::format("{:%Y%m%dT%H%M%S}Z", std::chrono::floor<std::chrono::seconds>(now));
}

// Copies `from` into a new directory `to` through a temporary sibling, so a
// slot is either complete or absent.
bool CopyTree(const fs::path& from, const fs::path& to) {
  std::error_code error;
  const fs::path staging = fs::path(to) += ".partial";
  fs::remove_all(staging, error);
  fs::create_directories(staging, error);
  for (auto it = fs::recursive_directory_iterator(from, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    const auto relative = it->path().lexically_relative(from);
    if (IsOtherPath(relative)) {
      it.disable_recursion_pending();
      continue;
    }
    const auto destination = staging / relative;
    if (it->is_directory(error)) fs::create_directories(destination, error);
    else fs::copy_file(it->path(), destination, fs::copy_options::none, error);
  }
  if (error) {
    REXLOG_ERROR("Save backup: copying {} failed: {}", rex::path_to_utf8(from), error.message());
    fs::remove_all(staging, error);
    return false;
  }
  fs::rename(staging, to, error);
#ifdef _WIN32
  // A newly copied profile can briefly have an open Windows directory handle.
  // Keep publication atomic, and fail normally if the lock does not clear.
  for (int retry = 0; error == std::errc::permission_denied && retry < 20; ++retry) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fs::rename(staging, to, error);
  }
#endif
  if (error) {
    REXLOG_ERROR("Save backup: publishing {} failed: {}", rex::path_to_utf8(to), error.message());
    diagnostics::RecordEvent("save.copy.failed",
                             {{"path", rex::path_to_utf8(to)}, {"error", error.message()}});
  }
  return !error;
}

}  // namespace

SaveBackups::SaveBackups(fs::path user_root, fs::path backup_root, size_t keep)
    : user_root_(std::move(user_root)), backup_root_(std::move(backup_root)), keep_(keep) {}

SaveBackups::~SaveBackups() { Stop(); }

void SaveBackups::Start() {
  if (!thread_.joinable()) {
    thread_ = std::thread(&SaveBackups::Run, this);
  }
}

void SaveBackups::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

SaveBackups::Signature SaveBackups::Scan(const fs::path& root) {
  Signature signature;
  std::error_code error;
  for (auto it = fs::recursive_directory_iterator(root, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    if (IsOtherPath(it->path().lexically_relative(root))) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(error)) {
      continue;
    }
    ++signature.files;
    signature.bytes += it->file_size(error);
    signature.newest = std::max<int64_t>(
        signature.newest, it->last_write_time(error).time_since_epoch().count());
  }
  return signature;
}

bool SaveBackups::Snapshot(const fs::path& user_root, const fs::path& backup_root,
                           const std::string& reason) {
  std::error_code error;
  if (!fs::exists(user_root, error)) {
    return false;
  }
  fs::create_directories(backup_root, error);
  const std::string name = UtcStamp() + (reason.empty() ? "" : "-" + reason);
  const fs::path slot = backup_root / name;
  if (fs::exists(slot, error) || !CopyTree(user_root, slot / "user")) {
    return false;
  }
  const Signature signature = Scan(slot / "user");
  REXLOG_INFO("Save backup {}: {} files, {} KB", name, signature.files, signature.bytes >> 10);
  diagnostics::RecordEvent("save.backup", {{"slot", name},
                                           {"files", std::to_string(signature.files)},
                                           {"bytes", std::to_string(signature.bytes)}});
  return true;
}

void SaveBackups::Prune() const {
  auto slots = List();
  std::error_code error;
  for (size_t i = keep_; i < slots.size(); ++i) {
    fs::remove_all(slots[i].path, error);
  }
}

std::vector<SaveBackups::Slot> SaveBackups::List() const {
  std::vector<Slot> slots;
  std::error_code error;
  for (auto it = fs::directory_iterator(backup_root_, error);
       !error && it != fs::directory_iterator(); it.increment(error)) {
    if (!it->is_directory(error) || it->path().extension() == ".partial" ||
        !fs::exists(it->path() / "user", error)) {
      continue;
    }
    const Signature signature = Scan(it->path() / "user");
    slots.push_back({rex::path_to_utf8(it->path().filename()), it->path(), signature.files,
                     signature.bytes});
  }
  std::sort(slots.begin(), slots.end(),
            [](const Slot& a, const Slot& b) { return a.name > b.name; });
  return slots;
}

bool SaveBackups::ScheduleRestore(const std::string& slot) const {
  std::error_code error;
  if (slot.find_first_of("/\\") != std::string::npos ||
      !fs::exists(backup_root_ / slot / "user", error)) {
    return false;
  }
  std::ofstream(backup_root_ / kRestoreFile) << slot;
  diagnostics::RecordEvent("save.restore.scheduled", {{"slot", slot}});
  return true;
}

bool SaveBackups::RestorePending() const {
  std::error_code error;
  return fs::exists(backup_root_ / kRestoreFile, error);
}

void SaveBackups::CancelRestore() const {
  std::error_code error;
  fs::remove(backup_root_ / kRestoreFile, error);
}

void SaveBackups::ApplyPendingRestore(const fs::path& user_root, const fs::path& backup_root) {
  std::error_code error;
  const fs::path request = backup_root / kRestoreFile;
  if (!fs::exists(request, error)) {
    return;
  }
  std::string slot;
  std::getline(std::ifstream(request), slot);
  // A request written by hand may carry a BOM or a line end.
  if (slot.rfind("\xEF\xBB\xBF", 0) == 0) slot.erase(0, 3);
  while (!slot.empty() && std::isspace(uint8_t(slot.back()))) slot.pop_back();
  fs::remove(request, error);
  const fs::path source = backup_root / slot / "user";
  if (slot.empty() || slot.find_first_of("/\\") != std::string::npos ||
      !fs::exists(source, error)) {
    REXLOG_ERROR("Save restore: backup {} is missing; nothing restored", slot);
    diagnostics::RecordEvent("save.restore.failed", {{"slot", slot}, {"reason", "missing"}});
    return;
  }
  // The files being replaced are kept too, so a restore can be undone.
  if (fs::exists(user_root, error) && !Snapshot(user_root, backup_root, "before-restore")) {
    REXLOG_ERROR("Save restore: could not back up the current files; nothing restored");
    diagnostics::RecordEvent("save.restore.failed", {{"slot", slot}, {"reason", "backup"}});
    return;
  }
  const fs::path staging = user_root / "backups" / "restoring";
  fs::remove_all(staging, error);
  if (!CopyTree(source, staging)) {
    REXLOG_ERROR("Save restore: could not copy backup {}", slot);
    diagnostics::RecordEvent("save.restore.failed", {{"slot", slot}, {"reason", "copy"}});
    return;
  }
  // Swap the profile folders only: the current ones move aside, the
  // backup's move in, and any failure puts the current ones back.
  const fs::path previous = user_root / "backups" / "replaced";
  fs::remove_all(previous, error);
  fs::create_directories(previous, error);
  const auto folders = [](const fs::path& root) {
    std::vector<fs::path> names;
    std::error_code list_error;
    for (auto it = fs::directory_iterator(root, list_error);
         !list_error && it != fs::directory_iterator(); it.increment(list_error)) {
      if (it->is_directory(list_error) && IsSaveFolder(it->path().filename())) {
        names.push_back(it->path().filename());
      }
    }
    return names;
  };
  const std::vector<fs::path> current = folders(user_root);
  const std::vector<fs::path> restored = folders(staging);
  std::vector<fs::path> moved_out, moved_in;
  bool failed = false;
  for (const auto& name : current) {
    fs::rename(user_root / name, previous / name, error);
    if (error) { failed = true; break; }
    moved_out.push_back(name);
  }
  for (const auto& name : restored) {
    if (failed) break;
    fs::rename(staging / name, user_root / name, error);
    if (error) { failed = true; break; }
    moved_in.push_back(name);
  }
  if (failed) {
    REXLOG_ERROR("Save restore: could not put backup {} in place: {}", slot, error.message());
    diagnostics::RecordEvent("save.restore.failed",
                             {{"slot", slot}, {"reason", "swap"}, {"error", error.message()}});
    std::error_code undo;
    for (const auto& name : moved_in) fs::rename(user_root / name, staging / name, undo);
    for (const auto& name : moved_out) fs::rename(previous / name, user_root / name, undo);
    return;
  }
  fs::remove_all(staging, error);
  fs::remove_all(previous, error);
  REXLOG_INFO("Save restore: restored backup {}", slot);
  diagnostics::RecordEvent("save.restore.applied", {{"slot", slot}});
}

void SaveBackups::Run() {
#ifdef _WIN32
  // Background mode lowers this thread's disk and memory priority, so the
  // copies queue behind the title's own streaming and saves.
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
  // A backup at the start of each session when the files differ from the
  // newest backup, then at most one per kMinSnapshotInterval after the title
  // saves, and a last one at exit when the files changed since.
  Signature saved;
  if (auto slots = List(); !slots.empty()) {
    saved = Scan(slots.front().path / "user");
  }
  Signature current = Scan(user_root_);
  if (current.files && !(current.files == saved.files && current.bytes == saved.bytes)) {
    if (Snapshot(user_root_, backup_root_, "session")) {
      Prune();
    }
  }
  saved = current;
  Signature previous = current;
  auto last_snapshot = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);
  while (!wake_.wait_for(lock, kPollInterval, [this] { return stopping_; })) {
    lock.unlock();
    current = Scan(user_root_);
    // Changed since the last backup, still for a whole poll interval, and
    // the previous backup is old enough.
    const auto now = std::chrono::steady_clock::now();
    if (current == previous && !(current == saved) && current.files &&
        now - last_snapshot >= kMinSnapshotInterval) {
      if (Snapshot(user_root_, backup_root_, "")) {
        Prune();
      }
      saved = current;
      last_snapshot = now;
    }
    previous = current;
    lock.lock();
  }
  lock.unlock();
  // Keep the progress since the last backup when the game closes normally.
  current = Scan(user_root_);
  if (current.files && !(current == saved) && Snapshot(user_root_, backup_root_, "exit")) {
    Prune();
  }
}

}  // namespace lavender_shift
