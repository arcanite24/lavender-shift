// Quality-of-life switches that are FH2 developer launch options
// (docs/PINYON_PARITY.md LP-2.7), set live the way Pinyon Shift sets FH1's:
// once the options object's constructor has run (its vtable word is in
// place), each wanted flag byte is held at 1, and the game's own value is
// put back when the setting is turned off.
//
// The object is TU v2 0x83427B80 with vtable 0x8201703C (FH1 v4 0x833D02E0
// and 0x82014B1C, mapped as data). The offsets are where the option
// parser's code for each name stores its flag; each sits 16 bytes before
// FH1 v4's.

#include <array>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "dev_options.h"
#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(lavender_skip_voiceover, false, "Lavender Shift",
                    "Let A skip voice-over dialogs");
REXCVAR_DEFINE_BOOL(lavender_no_attract_video, false, "Lavender Shift",
                    "Do not replay the intro video when the title screen is left idle");
REXCVAR_DEFINE_BOOL(lavender_no_auto_reset, false, "Lavender Shift",
                    "Do not reset the car to the road automatically when it leaves it");
REXCVAR_DEFINE_STRING(lavender_dev_option_bytes, "", "Lavender Shift",
                      "Diagnostic: hold option bytes, as offset=value pairs separated by "
                      "commas (for example 3076=1 for engageaionstart)");
REXCVAR_DEFINE_BOOL(lavender_photo_camera_through_walls, false, "Lavender Shift",
                    "Let the photo-mode camera pass through walls and objects");

namespace {

struct Option {
  const char* name;
  uint32_t offset;
};

constexpr uint32_t kObject = 0x83427B80u, kVtable = 0x8201703Cu;
constexpr std::array<Option, 4> kOptions = {{
    {"allowskipvoiceover", 4936},                  // FH1 v4 4952
    {"noattractvideo", 4222},                      // FH1 v4 4238
    {"NoHandOfGodReset", 4949},                    // FH1 v4 4965
    {"disablephotomodecollisiondetection", 4933},  // FH1 v4 4949
}};

struct State {
  int original = -1;  // the game's value, read before the first write
  bool written = false;
};

std::mutex g_mutex;
std::array<State, kOptions.size()> g_state;
bool g_any_written = false;

std::array<bool, kOptions.size()> Wanted() {
  return {REXCVAR_GET(lavender_skip_voiceover), REXCVAR_GET(lavender_no_attract_video),
          REXCVAR_GET(lavender_no_auto_reset), REXCVAR_GET(lavender_photo_camera_through_walls)};
}

// The diagnostic pairs, parsed again only when the string changes.
std::vector<std::pair<uint32_t, uint8_t>> RawBytes() {
  static std::string parsed_from;
  static std::vector<std::pair<uint32_t, uint8_t>> pairs;
  const std::string& text = REXCVAR_GET(lavender_dev_option_bytes);
  if (text == parsed_from) return pairs;
  parsed_from = text;
  pairs.clear();
  std::string_view rest = text;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view item = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    const size_t equals = item.find('=');
    if (equals == std::string_view::npos) continue;
    uint32_t offset = 0, value = 0;
    const auto a = std::from_chars(item.data(), item.data() + equals, offset);
    const auto b = std::from_chars(item.data() + equals + 1, item.data() + item.size(), value);
    if (a.ec != std::errc() || b.ec != std::errc() || offset >= 0x4000 || value > 255) continue;
    pairs.emplace_back(offset, uint8_t(value));
    REXLOG_INFO("Developer option byte {} held at {}", offset, value);
  }
  return pairs;
}

}  // namespace

namespace lavender_shift::dev_options {

void Update() {
  const auto wanted = Wanted();
  bool any_wanted = false;
  for (bool w : wanted) any_wanted |= w;
  std::lock_guard lock(g_mutex);
  const auto raw = RawBytes();
  if (!raw.empty() && guest::LoadU32(kObject) == kVtable) {
    for (const auto& [offset, value] : raw) {
      if (guest::LoadU8(kObject + offset) != value) guest::StoreU8(kObject + offset, value);
    }
  }
  if (!any_wanted && !g_any_written) return;
  // Before the constructor runs it would overwrite the bytes with defaults.
  if (const uint32_t vtable = guest::LoadU32(kObject); vtable != kVtable) {
    static unsigned misses = 0;
    if (misses++ % 600 == 0) {
      REXLOG_INFO("Developer options: object {:08X} holds {:08X}, not {:08X}", kObject, vtable,
                  kVtable);
    }
    return;
  }
  g_any_written = false;
  for (size_t i = 0; i < kOptions.size(); ++i) {
    const uint32_t flag = kObject + kOptions[i].offset;
    State& state = g_state[i];
    if (wanted[i]) {
      if (state.original < 0) state.original = guest::LoadU8(flag);
      if (guest::LoadU8(flag) != 1) guest::StoreU8(flag, 1);
      if (!state.written) {
        REXLOG_INFO("Developer option {} on (was {})", kOptions[i].name, state.original);
      }
      state.written = true;
      g_any_written = true;
    } else if (state.written) {
      guest::StoreU8(flag, uint8_t(state.original < 0 ? 0 : state.original));
      state.written = false;
      REXLOG_INFO("Developer option {} back to {}", kOptions[i].name, state.original);
    }
  }
}

}  // namespace lavender_shift::dev_options
