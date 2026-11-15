// Mod archives (docs/PINYON_PARITY.md LP-2.10), Pinyon Shift's hooks with
// FH2's sites (config/analysis/default.toml). FH2 checks some game files
// against a table in the executable: the file's size when it is opened, and
// each block's SHA-256 as it is read. A mismatch ends in the dirty-disc
// error, so a mod's rebuilt archive (larger, other bytes) would stop the
// game. The checks are accepted only for files a loaded mod replaces
// (mods::IsOverriddenGamePath); every other file is still checked.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

#include "diagnostics.h"
#include "guest_memory.h"
#include "mods/overlay_device.h"

namespace {

using namespace lavender_shift::guest;

// A NUL-terminated guest string of at most `limit` bytes.
std::string ReadAscii(uint32_t address, uint32_t limit) {
  std::string text;
  if (!address) return text;
  for (uint32_t n = 0; n < limit && Readable(address + n, 1); ++n) {
    const char c = char(LoadU8(address + n));
    if (!c) break;
    text.push_back(c);
  }
  return text;
}

void LogOnce(const char* event, const std::string& path,
             std::initializer_list<lavender_shift::diagnostics::Field> extra = {}) {
  static std::mutex mutex;
  static std::set<std::string> logged;
  {
    std::lock_guard lock(mutex);
    if (!logged.insert(std::string(event) + path).second) return;
  }
  std::vector<lavender_shift::diagnostics::Field> fields = {{"path", path}};
  fields.insert(fields.end(), extra.begin(), extra.end());
  lavender_shift::diagnostics::RecordEvent(event, fields);
}

}  // namespace

// r3: the block's check (low byte nonzero when it matched); r24: the file's
// table entry, its path at +32 relative to its root ("db\gamedb.slt").
void LavenderAcceptModdedBlock(PPCRegister& r3, PPCRegister& r24) {
  if (r3.u32 & 0xFFu) return;
  const std::string path = ReadAscii(LoadU32(r24.u32 + 32u), 260u);
  if (path.empty() || !lavender_shift::mods::IsOverriddenGamePath(path)) return;
  r3.u64 = 1;
  LogOnce("mod.file.hash_accepted", path);
}

void LavenderAcceptModdedLastBlock(PPCRegister& r3, PPCRegister& r24) {
  LavenderAcceptModdedBlock(r3, r24);
}

// r6: the opened file's size; r31: the table's, which becomes the stream's.
void LavenderAcceptModdedFileSize(PPCRegister& r1, PPCRegister& r6, PPCRegister& r31) {
  if (r6.u64 == r31.u64 || r6.u64 > 0xFFFFFFFFull) return;
  const std::string path = ReadAscii(LoadU32(r1.u32 + 92u), 260u);
  if (path.empty() || !lavender_shift::mods::IsOverriddenGamePath(path)) return;
  LogOnce("mod.file.size_accepted", path,
          {{"table_size", std::to_string(r31.u32)}, {"size", std::to_string(r6.u64)}});
  r31.u64 = r6.u64;
}
