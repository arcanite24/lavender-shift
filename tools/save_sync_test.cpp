// Tests of save sync (src/save_sync.cpp, docs/SAVE_SYNC.md): two devices'
// user folders and one shared sync folder in a temporary directory, the
// sync app being a no-op (both devices see the folder at once).
//
//   tools/build.ps1 builds the game; this test is built on its own:
//   cmake --build out/build/win-amd64-release --target lavender_shift_save_sync_test

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "diagnostics.h"
#include "save_backups.h"
#include "save_sync.h"

// The game's diagnostics are not linked here.
namespace lavender_shift::diagnostics {
void RecordEvent(std::string_view, std::initializer_list<Field>) {}
void RecordEvent(std::string_view, std::span<const Field>) {}
}  // namespace lavender_shift::diagnostics

namespace fs = std::filesystem;
using lavender_shift::SaveSync;

namespace {

int g_failures = 0;
#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                           \
    }                                                                         \
  } while (0)

constexpr const char* kProfile = "B13EBABEBABEBABE/4D530AA4/00000001/ForzaProfile";

void WriteFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// A save with a given progress marker, laid out like FH2's.
void MakeSave(const fs::path& user, const std::string& progress) {
  WriteFile(user / kProfile / "ForzaProfile", "profile " + progress);
  WriteFile(user / kProfile / "PlayerDatabase", std::string(4096, 'x') + progress);
  WriteFile(user / "B13EBABEBABEBABE/4D530AA4/Headers/00000001/ForzaProfile.header", "header");
  // Not part of the save.
  WriteFile(user / "0000000000000000/content.bin", "dlc");
  WriteFile(user / "cache/shader.bin", "cache " + progress);
}

std::string Progress(const fs::path& user) {
  return ReadFile(user / kProfile / "ForzaProfile");
}

void Achievements(const fs::path& user, std::initializer_list<std::pair<int, uint64_t>> unlocks) {
  std::string text = "# Achievement unlock state - managed by ReXGlue runtime\n";
  for (const auto& [id, time] : unlocks) {
    text += "\n[unlocked." + std::to_string(id) + "]\nfiletime = " + std::to_string(time) + "\n";
  }
  WriteFile(user / "achievements/4D530AA4.toml", text);
}

}  // namespace

int main() {
  std::random_device random;
  const fs::path root = fs::temp_directory_path() /
                        ("lavender-save-sync-test-" + std::to_string(random()));
  const fs::path a = root / "a", b = root / "b", folder = root / "sync";
  fs::create_directories(folder);

  // Archive round trip, and the hash ignores what is not the save.
  MakeSave(a, "1");
  const std::string hash1 = SaveSync::HashSave(a);
  CHECK(!hash1.empty());
  CHECK(SaveSync::WriteArchive(a, root / "test.lsave"));
  CHECK(SaveSync::ExtractArchive(root / "test.lsave", root / "extracted"));
  CHECK(SaveSync::HashSave(root / "extracted") == hash1);
  CHECK(!fs::exists(root / "extracted/cache"));
  CHECK(!fs::exists(root / "extracted/0000000000000000"));
  WriteFile(a / "cache/shader.bin", "changed");
  CHECK(SaveSync::HashSave(a) == hash1);

  // A has a save, B none: B loads A's without asking.
  Achievements(a, {{155, 100}, {156, 200}});
  SaveSync sync_a(a, folder, "Device A");
  auto start = sync_a.Startup();
  CHECK(!start.folder_unavailable);
  CHECK(start.imported_from.empty());
  CHECK(sync_a.Remotes().empty());
  SaveSync sync_b(b, folder, "Device B");
  start = sync_b.Startup();
  CHECK(start.imported_from == "Device A");
  CHECK(start.diverged.empty());
  CHECK(Progress(b) == "profile 1");
  CHECK(start.achievements_merged == 2);

  // B plays on: A fast-forwards to B's save, keeping its own in a backup.
  MakeSave(b, "2");
  CHECK(sync_b.Export(b));
  start = sync_a.Startup();
  CHECK(start.imported_from == "Device B");
  CHECK(start.diverged.empty());
  CHECK(Progress(a) == "profile 2");
  bool backed_up = false;
  for (const auto& entry : fs::directory_iterator(a / "backups/saves")) {
    if (entry.path().filename().string().ends_with("before-restore") &&
        ReadFile(entry.path() / "user" / kProfile / "ForzaProfile") == "profile 1") {
      backed_up = true;
    }
  }
  CHECK(backed_up);
  // Nothing new: no import, nothing to choose.
  start = sync_b.Startup();
  CHECK(start.imported_from.empty());
  CHECK(start.diverged.empty());

  // Both play without syncing: each sees the other's save as diverged.
  MakeSave(a, "3a");
  MakeSave(b, "3b");
  Achievements(b, {{155, 50}, {160, 300}});
  CHECK(sync_a.Export(a));
  CHECK(sync_b.Export(b));
  start = sync_a.Startup();
  CHECK(start.imported_from.empty());
  CHECK(start.diverged.size() == 1 && start.diverged[0].device_name == "Device B");
  CHECK(Progress(a) == "profile 3a");
  CHECK(start.achievements_merged == 1);  // 160
  CHECK(ReadFile(a / "achievements/4D530AA4.toml").find("filetime = 50") != std::string::npos);
  start = sync_b.Startup();
  CHECK(start.diverged.size() == 1 && start.diverged[0].device_name == "Device A");

  // A keeps its own: once A publishes, B loads A's save without asking.
  auto remotes = sync_a.Remotes();
  CHECK(remotes.size() == 1);
  CHECK(sync_a.KeepLocal(remotes[0].device_id));
  CHECK(sync_a.Export(a));
  CHECK(sync_a.Remotes()[0].relation == SaveSync::Relation::kLocalAhead);
  start = sync_b.Startup();
  CHECK(start.imported_from == "Device A");
  CHECK(start.diverged.empty());
  CHECK(Progress(b) == "profile 3a");

  // Diverge again; B chooses A's save on the SAVE SYNC page.
  MakeSave(a, "4a");
  MakeSave(b, "4b");
  CHECK(sync_a.Export(a));
  CHECK(sync_b.Export(b));
  remotes = sync_b.Remotes();
  CHECK(remotes.size() == 1 && remotes[0].relation == SaveSync::Relation::kDiverged);
  CHECK(sync_b.ScheduleImport(remotes[0].device_id));
  CHECK(sync_b.ImportPending().has_value());
  start = sync_b.Startup();
  CHECK(start.imported_from == "Device A");
  CHECK(Progress(b) == "profile 4a");
  CHECK(!sync_b.ImportPending().has_value());
  start = sync_a.Startup();
  CHECK(start.diverged.empty());

  // A damaged archive (a sync app's partial file) is ignored.
  MakeSave(a, "5");
  CHECK(sync_a.Export(a));
  remotes = sync_b.Remotes();
  CHECK(remotes.size() == 1);
  const fs::path archive_a = folder / "devices" / (remotes[0].device_id + ".lsave");
  const std::string good = ReadFile(archive_a);
  WriteFile(archive_a, good.substr(0, good.size() / 2) + std::string(good.size() - good.size() / 2, '\0'));
  CHECK(sync_b.Remotes().empty());
  start = sync_b.Startup();
  CHECK(start.imported_from.empty());
  CHECK(Progress(b) == "profile 4a");
  WriteFile(archive_a, good);
  start = sync_b.Startup();
  CHECK(start.imported_from == "Device A");
  CHECK(Progress(b) == "profile 5");

  // A new device with no save, while the others differ: nothing loads on
  // its own, and both saves are offered.
  MakeSave(a, "6a");
  MakeSave(b, "6b");
  CHECK(sync_a.Export(a));
  CHECK(sync_b.Export(b));
  const fs::path c = root / "c";
  SaveSync sync_c(c, folder, "Device C");
  start = sync_c.Startup();
  CHECK(start.imported_from.empty());
  CHECK(start.diverged.size() == 2);
  CHECK(!fs::exists(c / kProfile));
  CHECK(sync_c.ScheduleImport(start.diverged[0].device_id));
  start = sync_c.Startup();
  CHECK(!start.imported_from.empty());
  CHECK(fs::exists(c / kProfile / "ForzaProfile"));
  CHECK(start.diverged.size() == 1);

  // A sync app's conflict copy is never read as a device.
  WriteFile(folder / "devices" / (remotes[0].device_id + ".sync-conflict-20261114-1.toml"),
            ReadFile(folder / "devices" / (remotes[0].device_id + ".toml")));
  CHECK(sync_b.Remotes().size() == 2);  // A and C, not the copy

  // A folder that is missing (a drive not plugged in) is reported, not made.
  SaveSync missing(a, root / "not-there", "Device A");
  CHECK(missing.Startup().folder_unavailable);
  CHECK(!fs::exists(root / "not-there"));

  std::error_code error;
  fs::remove_all(root, error);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("save sync: all checks passed\n");
  return 0;
}
