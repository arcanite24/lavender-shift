#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lavender_shift::mods {

// Game data patches (issue #5 SR-16). A mod may ship patches.toml beside its
// mod.toml: edits to the game's XML files instead of whole files, so a pack
// that lengthens shadows or tunes fog does not redistribute the file and
// still applies over another mod's copy of it.
//
//   [[patch]]
//   file = "media/tracks/Anthem/TrackSettings.xml"   # on the game drive
//   element = "TrackSettings/ShadowEnds"             # from the root; Name[2]
//                                                    # picks the third sibling
//   attribute = "InGameShadowEnd"                    # default "value"
//   scale = 2.0                                      # or value = "..." / add = 5
//
// An element path without an index applies to every element with that path.
// Numbers keep the original's decimals. Each patch must match at least once,
// or it is reported. At start the patched files are written to the player's
// cache and served over the disc's and the mods' files; nothing in the game
// folder or the mods changes.

struct DataPatch {
  std::string file;
  std::string element;
  std::string attribute = "value";
  enum class Op { kSet, kScale, kAdd } op = Op::kSet;
  std::string value;  // kSet: the text; otherwise the number
  std::string source;  // "<mod>/patches.toml:<line>", for messages
};

// Appends the patches in `text`; returns the problems ("" fields, unknown
// keys), each naming `source` and the line.
std::vector<std::string> ParseDataPatches(std::string_view text, const std::string& source,
                                          std::vector<DataPatch>& patches);

// Applies `patches` (all for this file) to `xml` in order. The rest of the
// text is kept byte for byte. Returns the problems; a patch that matches no
// element or attribute is one.
std::vector<std::string> ApplyDataPatches(std::string& xml, const std::vector<DataPatch>& patches,
                                          int* applied = nullptr);

struct DataPatchResult {
  int files = 0;    // patched files written
  int applied = 0;  // attribute edits
  std::vector<std::string> problems;
};

// Reads every patches.toml in `mod_directories` (priority order, earlier
// wins), patches each named file as the game would read it (the first of
// `game_roots` that has it, else `base_root`), and writes the results under
// `out_root`, which is emptied first. Serve `out_root` ahead of `game_roots`.
DataPatchResult BuildDataPatches(const std::vector<std::filesystem::path>& mod_directories,
                                 const std::filesystem::path& base_root,
                                 const std::vector<std::filesystem::path>& game_roots,
                                 const std::filesystem::path& out_root);

}  // namespace lavender_shift::mods
