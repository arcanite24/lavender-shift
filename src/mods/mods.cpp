#include "mods/mods.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "config/host_config.h"
#include "diagnostics.h"

namespace lavender_shift::mods {

namespace {

std::vector<ModInfo> g_mods;

std::string Trim(std::string text) {
  text.erase(0, text.find_first_not_of(" \t\""));
  text.erase(text.find_last_not_of(" \t\"") + 1);
  return text;
}

// "a, b" or ["a", "b"]: the names, each once.
std::vector<std::string> SplitList(std::string text) {
  if (!text.empty() && text.front() == '[') text.erase(0, 1);
  if (!text.empty() && text.back() == ']') text.pop_back();
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    std::string name =
        Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(name);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return names;
}

struct Manifest {
  std::string version;
  std::vector<std::string> requires_mods, load_after, conflicts;
  bool shares_save = false;
};

bool ValidName(const std::string& name) {
  return !name.empty() && name.size() <= 64 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_' || c == '-';
         });
}

std::filesystem::path Directory(const std::filesystem::path& user_root, const std::string& name) {
  return user_root / "mods" / rex::to_path(name);
}

// The problem with the mod, or "".
std::string ReadManifest(const std::string& name, const std::filesystem::path& directory,
                         Manifest& manifest) {
  if (!ValidName(name)) return "the name may only use letters, digits, _ and -";
  const auto text = config::ReadFile(directory / "mod.toml");
  if (!text) return "mod.toml cannot be read";
  const auto get = [&](const char* key) { return config::GetValue(*text, key); };
  if (get("name").value_or("") != name) return "mod.toml names another mod";
  if (get("library")) return "native mods are not supported (Pinyon's mod ABI is FH1's)";
  manifest.version = get("version").value_or("0");
  manifest.shares_save = get("shares_save").value_or("false") == "true";
  manifest.requires_mods = SplitList(get("requires").value_or(""));
  manifest.load_after = SplitList(get("load_after").value_or(""));
  manifest.conflicts = SplitList(get("conflicts").value_or(""));
  return {};
}

}  // namespace

bool ShareSave(const std::filesystem::path& user_root, const std::string& enabled_mods) {
  const auto names = SplitList(enabled_mods);
  for (const auto& name : names) {
    Manifest manifest;
    if (!ReadManifest(name, Directory(user_root, name), manifest).empty() ||
        !manifest.shares_save) {
      return false;
    }
  }
  return true;
}

std::filesystem::path ModdedProfile(const std::filesystem::path& user_root) {
  auto root = user_root;
  if (!root.has_filename()) root = root.parent_path();
  return root.parent_path() / (root.filename().native() + rex::to_path("-modded").native());
}

void Load(const std::filesystem::path& user_root, const std::string& enabled_mods) {
  g_mods.clear();
  const std::vector<std::string> enabled = SplitList(enabled_mods);
  std::map<std::string, Manifest> manifests;
  for (const auto& name : enabled) {
    ModInfo info;
    info.name = name;
    info.directory = Directory(user_root, name);
    Manifest manifest;
    info.problem = ReadManifest(name, info.directory, manifest);
    info.version = manifest.version;
    if (info.problem.empty()) {
      for (const auto& other : manifest.conflicts) {
        if (std::find(enabled.begin(), enabled.end(), other) != enabled.end()) {
          info.problem = "conflicts with " + other;
        }
      }
      for (const auto& other : manifest.requires_mods) {
        if (std::find(enabled.begin(), enabled.end(), other) == enabled.end()) {
          info.problem = "requires " + other + ", which is not enabled";
        }
      }
    }
    if (info.problem.empty()) manifests.emplace(name, std::move(manifest));
    g_mods.push_back(std::move(info));
  }

  // Load order: after everything a mod requires or loads after; otherwise the
  // enabled_mods order. A mod whose requirement fails is not loaded either.
  std::vector<std::string> order;
  std::set<std::string> placed, visiting;
  std::function<bool(const std::string&)> place = [&](const std::string& name) -> bool {
    if (placed.count(name)) return true;
    if (!manifests.count(name) || visiting.count(name)) return false;
    visiting.insert(name);
    const Manifest& manifest = manifests.at(name);
    bool ok = true;
    for (const auto& before : manifest.requires_mods) ok = place(before) && ok;
    for (const auto& before : manifest.load_after) {
      if (manifests.count(before)) place(before);
    }
    visiting.erase(name);
    if (ok) {
      placed.insert(name);
      order.push_back(name);
    }
    return ok;
  };
  for (const auto& name : enabled) place(name);

  std::vector<ModInfo> ordered;
  for (const auto& name : order) {
    auto it = std::find_if(g_mods.begin(), g_mods.end(),
                           [&](const ModInfo& info) { return info.name == name; });
    it->loaded = true;
    ordered.push_back(*it);
  }
  for (const auto& info : g_mods) {
    if (!info.loaded) {
      ordered.push_back(info);
      if (ordered.back().problem.empty()) ordered.back().problem = "a mod it requires did not load";
    }
  }
  g_mods = std::move(ordered);

  for (const auto& info : g_mods) {
    if (info.loaded) {
      REXLOG_INFO("Mods: loaded {} {}", info.name, info.version);
      diagnostics::RecordEvent("mod.loaded", {{"name", info.name}, {"version", info.version}});
    } else {
      REXLOG_ERROR("Mods: {} was not loaded: {}", info.name, info.problem);
      diagnostics::RecordEvent("mod.rejected", {{"name", info.name}, {"reason", info.problem}});
    }
  }
}

const std::vector<ModInfo>& Mods() { return g_mods; }

std::vector<std::filesystem::path> Roots(std::string_view directory) {
  std::vector<std::filesystem::path> roots;
  std::error_code error;
  for (const auto& mod : g_mods) {
    if (!mod.loaded) continue;
    const auto root = mod.directory / rex::to_path(std::string(directory));
    if (std::filesystem::is_directory(root, error)) roots.push_back(root);
  }
  return roots;
}

}  // namespace lavender_shift::mods
