#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lavender_shift {

// A profile's folder in the user folder: 16 hex digits, not the all-zero
// folder that holds installed content. The saves are these folders.
bool IsSaveFolder(const std::filesystem::path& name);

// In-game save backups, Pinyon Shift's (docs/PINYON_PARITY.md LP-1.8). The
// saves are the profile folders in the user folder, one per XUID
// (B13EBABEBABEBABE for the default profile); the user folder's other
// entries (config, cache, photos, logs, installed content under
// 0000000000000000) are never copied or replaced. A background thread
// snapshots the saves once the title has written them and the files have
// been still for a few seconds (so a save in progress is never copied
// half-written), at most once per ten minutes and once more at exit, keeping
// the newest `keep` snapshots under `backup_root`. Restoring is only ever
// scheduled by the player and happens at the next start, before the title
// runs, after the current saves are themselves backed up.
class SaveBackups {
 public:
  struct Slot {
    std::string name;  // directory name: UTC time, plus a reason suffix
    std::filesystem::path path;
    uint64_t files = 0;
    uint64_t bytes = 0;
  };

  SaveBackups(std::filesystem::path user_root, std::filesystem::path backup_root,
              size_t keep = 10);
  ~SaveBackups();

  // Called on the backup thread after each new snapshot, with the snapshot's
  // copy of the user folder (the saves as they were when still). Set before
  // Start.
  void SetOnSnapshot(std::function<void(const std::filesystem::path& user_copy)> on_snapshot) {
    on_snapshot_ = std::move(on_snapshot);
  }

  void Start();
  void Stop();

  // Newest first.
  std::vector<Slot> List() const;
  // Restores `slot` at the next start; false when it does not exist.
  bool ScheduleRestore(const std::string& slot) const;
  bool RestorePending() const;
  void CancelRestore() const;

  // At startup, before the title runs: applies a scheduled restore.
  static void ApplyPendingRestore(const std::filesystem::path& user_root,
                                  const std::filesystem::path& backup_root);
  // Puts the saves of backup `slot` in place now, after backing up the
  // current ones; only while the title is not running.
  static bool RestoreSlot(const std::filesystem::path& user_root,
                          const std::filesystem::path& backup_root, const std::string& slot);
  // Copies `user_copy`'s saves into a new slot named after the time and
  // `reason`; returns the slot's name.
  static std::optional<std::string> AddSlot(const std::filesystem::path& user_copy,
                                            const std::filesystem::path& backup_root,
                                            const std::string& reason);

 private:
  struct Signature {
    uint64_t files = 0, bytes = 0;
    int64_t newest = 0;
    bool operator==(const Signature&) const = default;
  };
  static Signature Scan(const std::filesystem::path& root);
  // The new slot's copy of the user folder, or nullopt.
  static std::optional<std::filesystem::path> Snapshot(const std::filesystem::path& user_root,
                                                       const std::filesystem::path& backup_root,
                                                       const std::string& reason);
  bool TakeSnapshot(const std::string& reason);
  void Prune() const;
  void Run();

  std::filesystem::path user_root_;
  std::filesystem::path backup_root_;
  size_t keep_;
  std::function<void(const std::filesystem::path&)> on_snapshot_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
};

}  // namespace lavender_shift
