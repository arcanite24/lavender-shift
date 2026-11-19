#include "mods/data_patches.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace lavender_shift::mods {

namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return lower;
}

std::string_view TrimView(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool ParseNumber(std::string_view text, double& number) {
  const std::string copy(TrimView(text));
  if (copy.empty()) return false;
  char* end = nullptr;
  number = std::strtod(copy.c_str(), &end);
  return end == copy.c_str() + copy.size() && std::isfinite(number);
}

// The value of `key = value` with any quotes and a trailing comment removed.
std::string ParseValue(std::string_view text) {
  text = TrimView(text);
  if (!text.empty() && (text.front() == '"' || text.front() == '\'')) {
    const char quote = text.front();
    const size_t close = text.find(quote, 1);
    return std::string(text.substr(1, close == std::string_view::npos ? close : close - 1));
  }
  if (const size_t hash = text.find('#'); hash != std::string_view::npos) {
    text = TrimView(text.substr(0, hash));
  }
  return std::string(text);
}

// One element path segment: Name or Name[index].
struct Segment {
  std::string name;
  int index = -1;  // any
};

bool ParsePath(std::string_view path, std::vector<Segment>& segments) {
  segments.clear();
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    std::string_view part =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    Segment segment;
    if (const size_t bracket = part.find('['); bracket != std::string_view::npos) {
      if (part.back() != ']') return false;
      double index = 0;
      if (!ParseNumber(part.substr(bracket + 1, part.size() - bracket - 2), index) || index < 0 ||
          index != std::floor(index)) {
        return false;
      }
      segment.index = int(index);
      part = part.substr(0, bracket);
    }
    if (part.empty()) return false;
    segment.name = std::string(part);
    segments.push_back(std::move(segment));
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return !segments.empty();
}

bool IsNameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.' || c == ':';
}

// The attribute's value span inside the start tag [begin, end) of `xml`.
bool FindAttribute(const std::string& xml, size_t begin, size_t end, std::string_view attribute,
                   size_t& value_begin, size_t& value_end) {
  size_t i = begin;
  while (i < end && IsNameChar(xml[i])) ++i;  // the element's name
  while (i < end) {
    while (i < end && !IsNameChar(xml[i])) ++i;
    const size_t name_begin = i;
    while (i < end && IsNameChar(xml[i])) ++i;
    const std::string_view name(xml.data() + name_begin, i - name_begin);
    while (i < end && (xml[i] == ' ' || xml[i] == '\t' || xml[i] == '\r' || xml[i] == '\n')) ++i;
    if (i >= end || xml[i] != '=') continue;
    ++i;
    while (i < end && (xml[i] == ' ' || xml[i] == '\t' || xml[i] == '\r' || xml[i] == '\n')) ++i;
    if (i >= end || (xml[i] != '"' && xml[i] != '\'')) return false;
    const char quote = xml[i];
    const size_t close = xml.find(quote, i + 1);
    if (close == std::string::npos || close >= end) return false;
    if (name == attribute) {
      value_begin = i + 1;
      value_end = close;
      return true;
    }
    i = close + 1;
  }
  return false;
}

// `number` written like `original`: as many decimals, or an integer.
std::string FormatLike(double number, std::string_view original) {
  original = TrimView(original);
  const size_t dot = original.find('.');
  int decimals = 0;
  if (dot != std::string_view::npos) {
    size_t digits = dot + 1;
    while (digits < original.size() && original[digits] >= '0' && original[digits] <= '9') ++digits;
    decimals = int(digits - dot - 1);
  }
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", decimals, number);
  std::string formatted = text;
  if (formatted == "-0" || (formatted.rfind("-0.", 0) == 0 &&
                            formatted.find_first_not_of("-0.") == std::string::npos)) {
    formatted.erase(0, 1);
  }
  return formatted;
}

struct OpenElement {
  std::string name;
  std::map<std::string, int, std::less<>> children;
  int index = 0;
};

}  // namespace

std::vector<std::string> ParseDataPatches(std::string_view text, const std::string& source,
                                          std::vector<DataPatch>& patches) {
  std::vector<std::string> problems;
  std::vector<DataPatch> parsed;
  std::vector<int> lines;
  bool have_value = false;
  int line_number = 0;
  const auto finish = [&] {
    if (parsed.empty()) return;
    DataPatch& patch = parsed.back();
    const std::string where = patch.source;
    std::vector<Segment> segments;
    if (patch.file.empty()) {
      problems.push_back(where + ": no file");
    } else if (patch.element.empty() || !ParsePath(patch.element, segments)) {
      problems.push_back(where + ": element must be a path such as TrackSettings/ShadowEnds");
    } else if (!have_value) {
      problems.push_back(where + ": needs value, scale or add");
    } else {
      patches.push_back(patch);
    }
  };
  size_t start = 0;
  while (start < text.size()) {
    size_t newline = text.find('\n', start);
    std::string_view line =
        text.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                             : newline - start);
    start = newline == std::string_view::npos ? text.size() : newline + 1;
    ++line_number;
    line = TrimView(line);
    if (line.empty() || line.front() == '#') continue;
    const std::string where = source + ":" + std::to_string(line_number);
    if (line == "[[patch]]") {
      finish();
      parsed.emplace_back();
      parsed.back().source = where;
      have_value = false;
      continue;
    }
    const size_t equals = line.find('=');
    if (parsed.empty() || equals == std::string_view::npos) {
      problems.push_back(where + ": expected [[patch]] or key = value");
      continue;
    }
    DataPatch& patch = parsed.back();
    const std::string key(TrimView(line.substr(0, equals)));
    const std::string value = ParseValue(line.substr(equals + 1));
    if (key == "file") {
      patch.file = value;
    } else if (key == "element") {
      patch.element = value;
    } else if (key == "attribute") {
      patch.attribute = value;
    } else if (key == "value" || key == "scale" || key == "add") {
      double number = 0;
      if (have_value) {
        problems.push_back(where + ": only one of value, scale and add");
      } else if (key != "value" && !ParseNumber(value, number)) {
        problems.push_back(where + ": " + key + " must be a number");
      } else {
        patch.op = key == "value"   ? DataPatch::Op::kSet
                   : key == "scale" ? DataPatch::Op::kScale
                                    : DataPatch::Op::kAdd;
        patch.value = value;
        have_value = true;
      }
    } else {
      problems.push_back(where + ": unknown key " + key);
    }
  }
  finish();
  return problems;
}

std::vector<std::string> ApplyDataPatches(std::string& xml, const std::vector<DataPatch>& patches,
                                          int* applied) {
  std::vector<std::string> problems;
  for (const DataPatch& patch : patches) {
    std::vector<Segment> path;
    if (!ParsePath(patch.element, path)) {
      problems.push_back(patch.source + ": bad element path");
      continue;
    }
    int matched_elements = 0, edits = 0;
    std::string failure;
    std::vector<OpenElement> stack;
    std::map<std::string, int, std::less<>> top_level;
    size_t i = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
      if (xml.compare(i, 4, "<!--") == 0) {
        const size_t close = xml.find("-->", i + 4);
        i = close == std::string::npos ? xml.size() : close + 3;
        continue;
      }
      if (xml.compare(i, 9, "<![CDATA[") == 0) {
        const size_t close = xml.find("]]>", i + 9);
        i = close == std::string::npos ? xml.size() : close + 3;
        continue;
      }
      if (xml.compare(i, 2, "<?") == 0 || xml.compare(i, 2, "<!") == 0) {
        const size_t close = xml.find('>', i);
        i = close == std::string::npos ? xml.size() : close + 1;
        continue;
      }
      // The tag ends at the first '>' outside quotes.
      size_t end = i + 1;
      char quote = 0;
      for (; end < xml.size(); ++end) {
        if (quote) {
          if (xml[end] == quote) quote = 0;
        } else if (xml[end] == '"' || xml[end] == '\'') {
          quote = xml[end];
        } else if (xml[end] == '>') {
          break;
        }
      }
      if (end >= xml.size()) break;
      if (xml[i + 1] == '/') {
        if (!stack.empty()) stack.pop_back();
        i = end + 1;
        continue;
      }
      const bool self_closing = xml[end - 1] == '/';
      size_t name_end = i + 1;
      while (name_end < end && IsNameChar(xml[name_end])) ++name_end;
      OpenElement element;
      element.name = xml.substr(i + 1, name_end - i - 1);
      auto& siblings = stack.empty() ? top_level : stack.back().children;
      element.index = siblings[element.name]++;

      // Does the path (open elements plus this one) match the patch's?
      bool match = stack.size() + 1 == path.size();
      for (size_t level = 0; match && level < path.size(); ++level) {
        const OpenElement& open = level < stack.size() ? stack[level] : element;
        match = open.name == path[level].name &&
                (path[level].index < 0 || path[level].index == open.index);
      }
      if (match) {
        ++matched_elements;
        size_t value_begin = 0, value_end = 0;
        if (!FindAttribute(xml, i + 1, end, patch.attribute, value_begin, value_end)) {
          failure = "has no attribute " + patch.attribute;
        } else {
          const std::string original = xml.substr(value_begin, value_end - value_begin);
          std::string replacement;
          double number = 0, operand = 0;
          if (patch.op == DataPatch::Op::kSet) {
            replacement = patch.value;
          } else if (!ParseNumber(original, number) || !ParseNumber(patch.value, operand)) {
            failure = "attribute " + patch.attribute + " is not a number (" + original + ")";
          } else {
            replacement = FormatLike(
                patch.op == DataPatch::Op::kScale ? number * operand : number + operand, original);
          }
          if (!replacement.empty() || patch.op == DataPatch::Op::kSet) {
            const std::ptrdiff_t delta =
                std::ptrdiff_t(replacement.size()) - std::ptrdiff_t(original.size());
            xml.replace(value_begin, value_end - value_begin, replacement);
            end = size_t(std::ptrdiff_t(end) + delta);
            ++edits;
          }
        }
      }
      if (!self_closing) stack.push_back(std::move(element));
      i = end + 1;
    }
    if (!matched_elements) {
      problems.push_back(patch.source + ": no element " + patch.element + " in " + patch.file);
    } else if (!failure.empty()) {
      problems.push_back(patch.source + ": " + patch.element + " " + failure);
    }
    if (applied) *applied += edits;
  }
  return problems;
}

namespace {

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string PathText(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}

// `relative` under `root`, matching each part without regard to case (the
// game's paths are case-insensitive; Android's file system is not). Returns
// the path as it is on disk, relative to root, or empty.
std::filesystem::path FindFile(const std::filesystem::path& root, std::string_view relative) {
  std::filesystem::path found;
  std::error_code error;
  size_t start = 0;
  std::string text(relative);
  std::replace(text.begin(), text.end(), '\\', '/');
  while (start < text.size()) {
    size_t slash = text.find('/', start);
    const std::string part =
        text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    start = slash == std::string::npos ? text.size() : slash + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") return {};
    const auto directory = root / found;
    if (std::filesystem::exists(directory / Utf8Path(part), error)) {
      found /= Utf8Path(part);
      continue;
    }
    bool matched = false;
    for (std::filesystem::directory_iterator it(directory, error), last; !error && it != last;
         it.increment(error)) {
      if (Lower(PathText(it->path().filename())) == Lower(part)) {
        found /= it->path().filename();
        matched = true;
        break;
      }
    }
    if (!matched) return {};
  }
  return std::filesystem::is_regular_file(root / found, error) ? found : std::filesystem::path{};
}

bool ReadText(const std::filesystem::path& path, std::string& text) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  text = buffer.str();
  return true;
}

}  // namespace

DataPatchResult BuildDataPatches(const std::vector<std::filesystem::path>& mod_directories,
                                 const std::filesystem::path& base_root,
                                 const std::vector<std::filesystem::path>& game_roots,
                                 const std::filesystem::path& out_root) {
  DataPatchResult result;
  std::error_code error;
  std::filesystem::remove_all(out_root, error);

  // Later mods first, so an earlier mod's value is the one left.
  std::map<std::string, std::vector<DataPatch>> by_file;
  std::map<std::string, std::string> file_names;
  for (auto it = mod_directories.rbegin(); it != mod_directories.rend(); ++it) {
    const auto toml = *it / "patches.toml";
    std::string text;
    if (!std::filesystem::is_regular_file(toml, error) || !ReadText(toml, text)) continue;
    std::vector<DataPatch> patches;
    const std::string source = PathText(it->filename()) + "/patches.toml";
    for (auto& problem : ParseDataPatches(text, source, patches)) {
      result.problems.push_back(std::move(problem));
    }
    for (auto& patch : patches) {
      std::string key = patch.file;
      std::replace(key.begin(), key.end(), '\\', '/');
      key = Lower(key);
      file_names.emplace(key, patch.file);
      by_file[key].push_back(std::move(patch));
    }
  }

  for (const auto& [key, patches] : by_file) {
    const std::string& name = file_names[key];
    std::filesystem::path relative, source;
    for (const auto& root : game_roots) {
      if (relative = FindFile(root, name); !relative.empty()) {
        source = root / relative;
        break;
      }
    }
    if (source.empty()) {
      if (relative = FindFile(base_root, name); !relative.empty()) source = base_root / relative;
    }
    std::string xml;
    if (source.empty() || !ReadText(source, xml)) {
      result.problems.push_back(patches.front().source + ": no game file " + name);
      continue;
    }
    int applied = 0;
    for (auto& problem : ApplyDataPatches(xml, patches, &applied)) {
      result.problems.push_back(std::move(problem));
    }
    if (!applied) continue;
    const auto target = out_root / relative;
    std::filesystem::create_directories(target.parent_path(), error);
    std::ofstream out(target, std::ios::binary);
    out.write(xml.data(), std::streamsize(xml.size()));
    if (!out) {
      result.problems.push_back("cannot write " + PathText(target));
      continue;
    }
    ++result.files;
    result.applied += applied;
  }
  return result;
}

}  // namespace lavender_shift::mods
