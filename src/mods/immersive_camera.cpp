#include "mods/immersive_camera.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "diagnostics.h"
#include "guest_memory.h"
#include "ui/hostui/fh1_archive.h"

REXCVAR_DEFINE_BOOL(lavender_immersive_camera, true, "Lavender Shift",
                    "Add the immersive camera's layers to the game's camera physics at start "
                    "(the IMMERSIVE CAMERA rows scale them while the game runs)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(lavender_camera_gforce, 1.0, "Lavender Shift",
                      "Immersive camera: G-force push, pitch and jolt (0 off, 1 default, up to 2)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(lavender_camera_lean, 1.0, "Lavender Shift",
                      "Immersive camera: cockpit lean into corners (0 off, 1 default, up to 2)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(lavender_camera_shake, 1.0, "Lavender Shift",
                      "Immersive camera: cockpit speed shake and braking vibration (0 off, 1 "
                      "default, up to 2)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(lavender_camera_head, 1.0, "Lavender Shift",
                      "Immersive camera: cockpit breathing and head sway (0 off, 1 default, up "
                      "to 2)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::mods {
namespace {

struct CameraLayers {
  const char* camera;
  int count;
  const char* layers;
};

constexpr CameraLayers kLayers[] = {
#include "mods/immersive_camera_layers.inc"
};

// Bumped when the layers or the way the archive is built change, so a cached
// archive is built again.
constexpr int kBuildVersion = 2;

uint16_t Get16(const std::vector<uint8_t>& data, size_t at) {
  return uint16_t(data[at] | data[at + 1] << 8);
}
uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
         uint32_t(data[at + 3]) << 24;
}
void Put16(std::vector<uint8_t>& data, size_t at, uint16_t value) {
  data[at] = uint8_t(value);
  data[at + 1] = uint8_t(value >> 8);
}
void Put32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) data[at + i] = uint8_t(value >> (8 * i));
}

std::optional<std::vector<uint8_t>> ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// The disc's CameraPhysics.xml with the layers appended to each camera and
// its NumLayers raised, or nullopt when a camera is missing.
std::optional<std::string> AddLayers(std::string xml) {
  for (const auto& entry : kLayers) {
    const std::string open = std::string("<") + entry.camera + " NumLayers=\"";
    const size_t at = xml.find(open);
    if (at == std::string::npos) return std::nullopt;
    const size_t number = at + open.size();
    const size_t quote = xml.find('"', number);
    if (quote == std::string::npos) return std::nullopt;
    const int stock = std::atoi(xml.substr(number, quote - number).c_str());
    if (stock <= 0) return std::nullopt;
    xml.replace(number, quote - number, std::to_string(stock + entry.count));
    const std::string close = std::string("</") + entry.camera + ">";
    const size_t end = xml.find(close, number);
    if (end == std::string::npos) return std::nullopt;
    // At the start of the closing tag's line; the layers end with a newline.
    const size_t line = xml.rfind('\n', end);
    std::string_view layers(entry.layers);
    if (!layers.empty() && layers.front() == '\n') layers.remove_prefix(1);
    xml.insert(line == std::string::npos ? end : line + 1, layers);
  }
  return xml;
}

// The title's archives carry an extra field (id 0x1123, 4 bytes) in each
// central record with the absolute offset of the member's data, which it
// reads instead of walking the local header.
void SetDataOffset(std::vector<uint8_t>& record, uint32_t offset) {
  size_t at = 46 + size_t(Get16(record, 28));
  const size_t end = std::min(record.size(), at + Get16(record, 30));
  while (at + 4 <= end) {
    const uint16_t key = Get16(record, at);
    const uint16_t size = Get16(record, at + 2);
    if (key == 0x1123 && size == 4) {
      Put32(record, at + 4, offset);
      return;
    }
    at += 4 + size_t(size);
  }
}

struct Rebuilt {
  std::vector<uint8_t> data;
  uint32_t directory_start = 0;
  uint32_t directory_size = 0;  // with the end record, as zipmanifest.xml counts it
  uint16_t entries = 0;
};

// The ZIP `source` with the member `name` replaced by `content`, stored. The
// other members' bytes, headers and order are copied as they are (XMem LZX
// members stay compressed); FH2's reader wants the end record last.
std::optional<Rebuilt> ReplaceMember(const std::vector<uint8_t>& source, std::string_view name,
                                     const std::vector<uint8_t>& content) {
  constexpr size_t kEndSize = 22;
  if (source.size() < kEndSize) return std::nullopt;
  const size_t end = source.size() - kEndSize;
  if (Get32(source, end) != 0x06054B50) return std::nullopt;
  const uint16_t count = Get16(source, end + 10);
  const uint32_t directory_size = Get32(source, end + 12);
  const uint32_t directory_offset = Get32(source, end + 16);
  if (size_t(directory_offset) + directory_size > end) return std::nullopt;

  std::vector<uint8_t> out;
  std::vector<uint8_t> directory;
  bool replaced = false;
  size_t at = directory_offset;
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 46 > end || Get32(source, at) != 0x02014B50) return std::nullopt;
    const size_t record_size =
        46 + size_t(Get16(source, at + 28)) + Get16(source, at + 30) + Get16(source, at + 32);
    std::vector<uint8_t> record(source.begin() + at, source.begin() + at + record_size);
    const std::string_view member(reinterpret_cast<const char*>(&source[at + 46]),
                                  Get16(source, at + 28));
    const uint32_t local = Get32(source, at + 42);
    if (size_t(local) + 30 > source.size() || Get32(source, local) != 0x04034B50) {
      return std::nullopt;
    }
    const size_t header_size = 30 + size_t(Get16(source, local + 26)) + Get16(source, local + 28);
    const uint32_t compressed = Get32(source, at + 20);
    if (size_t(local) + header_size + compressed > source.size()) return std::nullopt;
    std::vector<uint8_t> header(source.begin() + local, source.begin() + local + header_size);
    Put32(record, 42, uint32_t(out.size()));
    SetDataOffset(record, uint32_t(out.size() + header_size));
    if (member == name) {
      const uint32_t crc = hostui::Crc32(content);
      const auto size = uint32_t(content.size());
      Put16(header, 8, 0);
      Put32(header, 14, crc);
      Put32(header, 18, size);
      Put32(header, 22, size);
      Put16(record, 10, 0);
      Put32(record, 16, crc);
      Put32(record, 20, size);
      Put32(record, 24, size);
      out.insert(out.end(), header.begin(), header.end());
      out.insert(out.end(), content.begin(), content.end());
      replaced = true;
    } else {
      out.insert(out.end(), header.begin(), header.end());
      out.insert(out.end(), source.begin() + local + header_size,
                 source.begin() + local + header_size + compressed);
    }
    directory.insert(directory.end(), record.begin(), record.end());
    at += record_size;
  }
  if (!replaced) return std::nullopt;
  std::vector<uint8_t> tail(source.begin() + end, source.end());
  Rebuilt rebuilt;
  rebuilt.directory_start = uint32_t(out.size());
  rebuilt.directory_size = uint32_t(directory.size() + tail.size());
  rebuilt.entries = count;
  Put32(tail, 12, uint32_t(directory.size()));
  Put32(tail, 16, uint32_t(out.size()));
  out.insert(out.end(), directory.begin(), directory.end());
  out.insert(out.end(), tail.begin(), tail.end());
  rebuilt.data = std::move(out);
  return rebuilt;
}

// FH2 reads each archive's central directory from its line in
// media/zipmanifest.xml, not from the archive: the manifest with the line for
// `guest_path` given the rebuilt archive's directory, or nullopt when the
// line is missing.
std::optional<std::string> UpdateManifest(std::string text, std::string_view guest_path,
                                          const Rebuilt& rebuilt) {
  const std::string key = "path=\"" + std::string(guest_path) + "\"";
  const size_t at = text.find(key);
  if (at == std::string::npos) return std::nullopt;
  const size_t start = text.rfind("<Zip", at);
  const size_t end = text.find("/>", at);
  if (start == std::string::npos || end == std::string::npos) return std::nullopt;
  std::string line = text.substr(start, end - start);
  for (const auto& [name, value] :
       {std::pair{"dirstart", rebuilt.directory_start}, std::pair{"dirsize", rebuilt.directory_size},
        std::pair{"direntries", uint32_t(rebuilt.entries)}}) {
    const std::string field = std::string(" ") + name + "=\"";
    const size_t field_at = line.find(field);
    if (field_at == std::string::npos) return std::nullopt;
    const size_t number = field_at + field.size();
    const size_t quote = line.find('"', number);
    if (quote == std::string::npos) return std::nullopt;
    line.replace(number, quote - number, std::to_string(value));
  }
  text.replace(start, end - start, line);
  return text;
}

std::string Stamp(std::initializer_list<std::filesystem::path> sources) {
  std::string stamp = std::to_string(kBuildVersion);
  for (const auto& source : sources) {
    std::error_code error;
    const auto size = std::filesystem::file_size(source, error);
    const auto time = std::filesystem::last_write_time(source, error).time_since_epoch().count();
    // libc++ (Android) counts file times in __int128, which to_string lacks.
    stamp += " " + std::to_string(size) + " " + std::to_string(static_cast<long long>(time));
  }
  return stamp;
}

}  // namespace

std::filesystem::path BuildImmersiveCamera(const std::filesystem::path& game_data_root,
                                           const std::filesystem::path& cache_root) {
  if (!REXCVAR_GET(lavender_immersive_camera)) return {};
  const auto source = game_data_root / "media" / "camera.zip";
  const auto root = cache_root / "immersive_camera";
  const auto manifest_source = game_data_root / "media" / "zipmanifest.xml";
  const auto target = root / "media" / "camera.zip";
  const auto manifest_target = root / "media" / "zipmanifest.xml";
  const auto stamp_path = root / "source.txt";
  const std::string stamp = Stamp({source, manifest_source});
  std::error_code error;
  if (std::filesystem::exists(target, error) && std::filesystem::exists(manifest_target, error)) {
    std::ifstream file(stamp_path);
    std::string saved;
    std::getline(file, saved);
    if (saved == stamp) return root;
  }
  const auto fail = [&](const char* why) {
    REXLOG_WARN("Immersive camera: {}", why);
    diagnostics::RecordEvent("camera.immersive.build_failed", {{"reason", why}});
    return std::filesystem::path();
  };
  const auto archive = hostui::Fh1Archive::Open(source);
  if (!archive) return fail("the disc's media/camera.zip could not be opened");
  const auto stock = archive->Read("CameraPhysics.xml");
  if (!stock) return fail("CameraPhysics.xml could not be read");
  const auto xml = AddLayers(std::string(stock->begin(), stock->end()));
  if (!xml) return fail("CameraPhysics.xml does not have the five cameras");
  const auto zip = ReadAll(source);
  if (!zip) return fail("the disc's media/camera.zip could not be read");
  const auto rebuilt =
      ReplaceMember(*zip, "CameraPhysics.xml", std::vector<uint8_t>(xml->begin(), xml->end()));
  if (!rebuilt) return fail("media/camera.zip has an unexpected layout");
  const auto manifest_bytes = ReadAll(manifest_source);
  if (!manifest_bytes) return fail("media/zipmanifest.xml could not be read");
  const auto manifest = UpdateManifest(std::string(manifest_bytes->begin(), manifest_bytes->end()),
                                       "game:\\media\\camera.zip", *rebuilt);
  if (!manifest) return fail("media/zipmanifest.xml does not list media/camera.zip");
  std::filesystem::create_directories(target.parent_path(), error);
  const auto write = [](const std::filesystem::path& path, const char* data, size_t size) {
    auto temporary = path;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(data, std::streamsize(size));
      if (!file) return false;
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    return !rename_error;
  };
  if (!write(target, reinterpret_cast<const char*>(rebuilt->data.data()), rebuilt->data.size()) ||
      !write(manifest_target, manifest->data(), manifest->size())) {
    return fail("the rebuilt files could not be written");
  }
  std::ofstream(stamp_path, std::ios::trunc) << stamp << "\n";
  diagnostics::RecordEvent("camera.immersive.built",
                           {{"path", rex::path_to_utf8(target)},
                            {"size", std::to_string(rebuilt->data.size())},
                            {"dirstart", std::to_string(rebuilt->directory_start)}});
  return root;
}

}  // namespace lavender_shift::mods

// sub_829C4xxx (FH1 v4 sub_82907F88, exact) updates one camera physics layer
// at r31 and has stored the factor for its Param at [r1+96]. The immersive
// camera's layers carry their effect group in the fraction of SpringK
// (sprung) or NoiseSeed (noise): 0.0419 + 0.0002 * group, as
// tools/build-immersive-camera.py writes them. No stock layer has such a
// fraction. Scaling the factor eases a sprung layer to its new strength.
void LavenderImmersiveCameraLayer(PPCRegister& r1, PPCRegister& r31) {
  using namespace lavender_shift::guest;
  const double scales[] = {REXCVAR_GET(lavender_camera_gforce), REXCVAR_GET(lavender_camera_lean),
                           REXCVAR_GET(lavender_camera_shake), REXCVAR_GET(lavender_camera_head)};
  if (std::all_of(std::begin(scales), std::end(scales), [](double s) { return s == 1.0; })) {
    return;
  }
  const uint32_t layer = r31.u32;
  // Sprung (+248), SpringK (+252); NoiseSeed (+156).
  const float tagged = LoadF32(layer + (LoadU8(layer + 248) ? 252u : 156u));
  if (!(tagged > 0.0f && tagged < 100000.0f)) return;
  const double fraction = double(tagged) - std::floor(double(tagged));
  const double group = (fraction - 0.0419) / 0.0002;
  const double nearest = std::round(group);
  if (nearest < 1.0 || nearest > 4.0 || std::abs(group - nearest) > 0.25) return;
  const size_t index = size_t(nearest) - 1;
  const double scale = scales[index];
  if (scale == 1.0) return;
  // Once per group, so a log shows the layers were found.
  static std::atomic<uint32_t> reported{};
  if (!(reported.fetch_or(1u << index, std::memory_order_relaxed) & (1u << index))) {
    static constexpr const char* kGroups[] = {"gforce", "lean", "shake", "head"};
    lavender_shift::diagnostics::RecordEvent(
        "camera.immersive.scaled", {{"group", kGroups[index]}, {"scale", std::to_string(scale)}});
  }
  const uint32_t factor = r1.u32 + 96u;
  StoreF32(factor, float(LoadF32(factor) * scale));
}
