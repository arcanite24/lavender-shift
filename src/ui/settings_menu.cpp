#include "ui/settings_menu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/input/pad_remap.h>
#include <rex/input/sdl/joystick_mapping.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/sdl/wheel.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/vulkan/presenter.h>

#include "config/default_keys.h"
#include "save_backups.h"
#include "diagnostics.h"

namespace lavender_shift::ui {
namespace {

using hostui::MenuRow;
using hostui::MenuScreen;

// One choice of a setting: the label shown and the TOML literal each
// setting takes (several for choices such as the resolution scale, which
// sets both axes).
struct Choice {
  std::string label;
  std::vector<std::pair<std::string, std::string>> values;
};

std::string Unquote(std::string_view literal) {
  if (literal.size() >= 2 && literal.front() == '"' && literal.back() == '"') {
    literal = literal.substr(1, literal.size() - 2);
  }
  return std::string(literal);
}

// Whole-string number, so "-0.5" and "-0.500000" compare equal.
std::optional<double> Number(std::string_view text) {
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (copy.empty() || end != copy.c_str() + copy.size()) {
    return std::nullopt;
  }
  return value;
}

bool SameValue(std::string_view a, std::string_view b) {
  const auto number_a = Number(a);
  const auto number_b = Number(b);
  if (number_a && number_b) {
    return *number_a == *number_b;
  }
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}

bool IsTemporalEffect(std::string_view effect) {
  return SameValue(effect, "fsr2") || SameValue(effect, "fsr3") || SameValue(effect, "dlss");
}

std::string TemporalUpscalerLabel(std::string_view id) {
  return SameValue(id, "fsr2")   ? "FSR 2"
         : SameValue(id, "fsr3") ? "FSR 3"
         : SameValue(id, "dlss") ? "DLSS"
                                 : Upper(std::string(id));
}

// Screens of one opening of the menu. The root screen's rows own the pages,
// so each function here may capture `this` through shared_from_this.
class SettingsPages : public std::enable_shared_from_this<SettingsPages> {
 public:
  SettingsPages(hostui::HostUi& host_ui, config::HostConfig& config, SettingsServices services)
      : host_ui_(host_ui), config_(config), services_(std::move(services)) {}

  std::unique_ptr<MenuScreen> Root();

 private:
  static std::string Live(const std::string& name) { return rex::cvar::GetFlagByName(name); }
  // The value in the settings file, or the running one.
  std::string Saved(const std::string& name) const {
    return config_.Get(name).value_or(Live(name));
  }

  static int Find(const std::vector<Choice>& choices) {
    for (size_t i = 0; i < choices.size(); ++i) {
      bool all = true;
      for (const auto& [name, literal] : choices[i].values) {
        all = all && SameValue(Live(name), Unquote(literal));
      }
      if (all) {
        return int(i);
      }
    }
    return -1;
  }

  MenuRow Setting(std::string label, std::vector<Choice> choices);
  MenuRow Toggle(std::string label, std::string name, bool inverted = false);
  MenuRow Page(std::string label, std::unique_ptr<MenuScreen> (SettingsPages::*page)());
  std::string ResolutionLine() const;
  bool TemporalUpscalerAvailable(std::string_view id) const {
    if (!services_.upscalers) return false;
    for (const rex::ui::UpscalerInfo& info : services_.upscalers()) {
      if (SameValue(info.id, id)) return info.available;
    }
    return false;
  }
  void Save();

  std::unique_ptr<MenuScreen> Display();
  std::unique_ptr<MenuScreen> Hud();
  std::unique_ptr<MenuScreen> HdrCalibration();
  std::unique_ptr<MenuScreen> Graphics();
  std::unique_ptr<MenuScreen> Audio();
  std::unique_ptr<MenuScreen> Gameplay();
  std::unique_ptr<MenuScreen> Controls();
  std::unique_ptr<MenuScreen> ControllerButtons();
  std::unique_ptr<MenuScreen> MapController();
  std::unique_ptr<MenuScreen> MapControllerOptions(rex::input::sdl::JoystickInfo joystick);
  std::unique_ptr<MenuScreen> MapControllerSteps(rex::input::sdl::JoystickInfo joystick);
  std::unique_ptr<MenuScreen> Wheel();
  std::unique_ptr<MenuScreen> Profile();
  std::unique_ptr<MenuScreen> Backups();
  std::unique_ptr<MenuScreen> ConfirmRestore(std::string slot);

  hostui::HostUi& host_ui_;
  config::HostConfig& config_;
  SettingsServices services_;
};

// The row shows the choice that matches the running values (CUSTOM when
// none does) and applies the next one at once: every value is set live and
// saved. A setting the SDK still marks as needing a restart is logged, since
// the menu must not offer one (it would silently do nothing until then).
MenuRow SettingsPages::Setting(std::string label, std::vector<Choice> choices) {
  for (const auto& choice : choices) {
    for (const auto& [name, literal] : choice.values) {
      const auto* flag = rex::cvar::GetFlagInfo(name);
      if (!flag) {
        REXLOG_WARN("Settings: {} offers unknown setting {}", label, name);
      } else if (flag->lifecycle != rex::cvar::Lifecycle::kHotReload) {
        REXLOG_WARN("Settings: {} sets {}, which does not apply live", label, name);
      }
    }
  }
  MenuRow row;
  row.label = std::move(label);
  auto shared = std::make_shared<std::vector<Choice>>(std::move(choices));
  row.value = [shared] {
    const int index = Find(*shared);
    return index >= 0 ? (*shared)[size_t(index)].label : std::string("CUSTOM");
  };
  row.adjust = [this, shared, label = row.label](int direction) {
    const auto& choices = *shared;
    const int count = int(choices.size());
    const int current = Find(choices);
    const int next = current < 0 ? 0 : (current + direction + count) % count;
    std::string values;
    for (const auto& [name, literal] : choices[size_t(next)].values) {
      config_.Set(name, literal);
      rex::cvar::SetFlagByName(name, Unquote(literal));
      values += (values.empty() ? "" : " ") + name + "=" + Unquote(literal);
    }
    diagnostics::RecordEvent("settings.changed", {{"row", label},
                                                  {"choice", choices[size_t(next)].label},
                                                  {"values", values}});
    Save();
  };
  return row;
}

MenuRow SettingsPages::Toggle(std::string label, std::string name, bool inverted) {
  const char* on = inverted ? "false" : "true";
  const char* off = inverted ? "true" : "false";
  return Setting(std::move(label), {{"OFF", {{name, off}}}, {"ON", {{name, on}}}});
}

MenuRow SettingsPages::Page(std::string label,
                            std::unique_ptr<MenuScreen> (SettingsPages::*page)()) {
  MenuRow row;
  row.label = std::move(label);
  row.activate = [self = shared_from_this(), page] { self->host_ui_.Push(((*self).*page)()); };
  return row;
}

void SettingsPages::Save() {
  if (!config_.Save()) {
    REXLOG_ERROR("Settings: could not write {}", rex::path_to_utf8(config_.path()));
  }
}

std::string SettingsPages::ResolutionLine() const {
  const auto number = [](const std::string& text, uint32_t fallback) {
    const auto value = Number(text);
    return value && *value >= 1 ? uint32_t(*value) : fallback;
  };
  uint32_t scale = services_.draw_resolution_scale ? services_.draw_resolution_scale() : 0;
  if (!scale) {
    scale = number(Live("draw_resolution_scale_x"), 1);
  }
  const uint32_t render_width = number(Live("video_mode_width"), 1280) * scale;
  const uint32_t render_height = number(Live("video_mode_height"), 720) * scale;
  const auto size = [](uint32_t width, uint32_t height) {
    return std::to_string(width) + "X" + std::to_string(height);
  };
  std::string line = "RENDERS " + size(render_width, render_height);
  const auto output = services_.output_size ? services_.output_size() : std::nullopt;
  if (!output || !output->first || !output->second) {
    return line;
  }
  const auto [output_width, output_height] = *output;
  const std::string effect = Unquote(Live("present_effect"));
  if (output_width == render_width && output_height == render_height) {
    if (IsTemporalEffect(effect) && TemporalUpscalerAvailable(effect)) {
      return line + ", " + TemporalUpscalerLabel(effect) + " ANTI-ALIASES AT THAT SIZE";
    }
    return line + ", SHOWN AT THAT SIZE";
  }
  std::string upscaler = SameValue(effect, "fsr") ? ", FSR 1 UPSCALES TO "
                         : IsTemporalEffect(effect)
                             ? ", " + TemporalUpscalerLabel(effect) + " UPSCALES TO "
                             : std::string(", STRETCHED TO ");
  std::string unavailable;
  if (IsTemporalEffect(effect) && !TemporalUpscalerAvailable(effect)) {
    upscaler = ", FSR 1 UPSCALES TO ";
    unavailable = " (" + TemporalUpscalerLabel(effect) + " UNAVAILABLE)";
  }
  if (output_width * output_height > render_width * render_height) {
    return line + upscaler + size(output_width, output_height) + unavailable;
  }
  return line + ", DOWNSCALED TO " + size(output_width, output_height);
}

std::unique_ptr<MenuScreen> SettingsPages::Display() {
  std::vector<MenuRow> rows;
  rows.push_back(Toggle("FULLSCREEN", "fullscreen"));
  // Both apply live (the SDK's ApplyConfiguredSizeAndMonitor).
  rows.push_back(Setting("MONITOR",
                         {{"DEFAULT", {{"monitor", "0"}}},
                          {"1", {{"monitor", "1"}}},
                          {"2", {{"monitor", "2"}}},
                          {"3", {{"monitor", "3"}}}}));
  {
    std::vector<Choice> sizes = {{"DEFAULT", {{"window_width", "0"}, {"window_height", "0"}}}};
    for (const auto& [width, height] : {std::pair{1280, 720}, std::pair{1600, 900},
                                        std::pair{1920, 1080}, std::pair{2560, 1440},
                                        std::pair{3840, 2160}}) {
      sizes.push_back({std::to_string(width) + "X" + std::to_string(height),
                       {{"window_width", std::to_string(width)},
                        {"window_height", std::to_string(height)}}});
    }
    rows.push_back(Setting("WINDOW SIZE", std::move(sizes)));
  }
  // WIDER VIEW widens the gameplay view to the window (Hor+, src/wide_view.cpp)
  // and switches the letterbox itself between it and the game's 16:9 screens.
  rows.push_back(Setting("ASPECT RATIO",
                         {{"LETTERBOX", {{"lavender_hor_plus", "false"},
                                         {"present_letterbox", "true"},
                                         {"present_allow_overscan_cutoff", "false"}}},
                          {"CROP", {{"lavender_hor_plus", "false"},
                                    {"present_letterbox", "true"},
                                    {"present_allow_overscan_cutoff", "true"}}},
                          {"STRETCH", {{"lavender_hor_plus", "false"},
                                       {"present_letterbox", "false"},
                                       {"present_allow_overscan_cutoff", "false"}}},
                          {"WIDER VIEW", {{"lavender_hor_plus", "true"},
                                          {"present_allow_overscan_cutoff", "false"}}}}));
  rows.push_back(Toggle("VSYNC", "vsync"));
  {
    // Every camera's vertical field of view (src/graphics_hooks.cpp).
    std::vector<Choice> fov;
    for (const char* value : {"0.9", "1.0", "1.1", "1.2", "1.3"}) {
      const int percent = int(std::lround(std::stod(value) * 100));
      fov.push_back({std::to_string(percent) + "%", {{"lavender_fov_scale", value}}});
    }
    rows.push_back(Setting("FIELD OF VIEW", std::move(fov)));
  }
  rows.push_back(Page("HUD", &SettingsPages::Hud));
  // The rate the game simulates and renders at, which sets most of its CPU
  // cost. 30 is the console's own rate; DISPLAY renders 60 on a 60 Hz or
  // faster display (two guest vblanks a frame).
  rows.push_back(Setting("GAME FRAME RATE",
                         {{"30 (CONSOLE)", {{"pinyon_shift_fh1_render_fps_limit", "30"}}},
                          {"40", {{"pinyon_shift_fh1_render_fps_limit", "40"}}},
                          {"60", {{"pinyon_shift_fh1_render_fps_limit", "60"}}},
                          {"DISPLAY", {{"pinyon_shift_fh1_render_fps_limit", "0"}}}}));
  rows.push_back(Setting("FRAME RATE LIMIT",
                         {{"OFF", {{"host_present_fps_limit", "0"}}},
                          {"30", {{"host_present_fps_limit", "30"}}},
                          {"60", {{"host_present_fps_limit", "60"}}},
                          {"120", {{"host_present_fps_limit", "120"}}}}));
  {
    std::vector<Choice> scaling = {{"BILINEAR", {{"present_effect", "\"bilinear\""}}},
                                   {"CAS", {{"present_effect", "\"cas\""}}},
                                   {"FSR 1", {{"present_effect", "\"fsr\""}}}};
    std::vector<rex::ui::UpscalerInfo> upscalers =
        services_.upscalers ? services_.upscalers() : std::vector<rex::ui::UpscalerInfo>{};
    std::stable_sort(upscalers.begin(), upscalers.end(),
                     [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const rex::ui::UpscalerInfo& info : upscalers) {
      if (info.kind == rex::ui::UpscalerKind::kTemporal && info.available) {
        scaling.push_back(
            {TemporalUpscalerLabel(info.id), {{"present_effect", "\"" + info.id + "\""}}});
      }
    }
    rows.push_back(Setting("OUTPUT SCALING", std::move(scaling)));
  }
  rows.push_back(Setting("SHARPENING", {{"OFF", {{"present_fsr_sharpness_reduction", "2.0"}}},
                                        {"LOW", {{"present_fsr_sharpness_reduction", "1.0"}}},
                                        {"MEDIUM", {{"present_fsr_sharpness_reduction", "0.5"}}},
                                        {"HIGH", {{"present_fsr_sharpness_reduction", "0.2"}}},
                                        {"MAX", {{"present_fsr_sharpness_reduction", "0.0"}}}}));
  const rex::ui::vulkan::HdrOutputState hdr = rex::ui::vulkan::GetHdrOutputState();
  if (hdr.offered || hdr.active || SameValue(Live("present_hdr"), "true")) {
    rows.push_back(Toggle("HDR", "present_hdr"));
    std::vector<Choice> paper_white;
    for (const char* nits : {"100", "150", "200", "250", "300", "400"}) {
      paper_white.push_back(
          {std::string(nits) + " NITS", {{"present_hdr_paper_white_nits", nits}}});
    }
    rows.push_back(Setting("HDR PAPER WHITE", std::move(paper_white)));
    rows.push_back(Setting("HDR HIGHLIGHTS", {{"OFF", {{"present_hdr_highlights", "0.0"}}},
                                              {"LOW", {{"present_hdr_highlights", "0.25"}}},
                                              {"MEDIUM", {{"present_hdr_highlights", "0.5"}}},
                                              {"HIGH", {{"present_hdr_highlights", "1.0"}}}}));
    // 2.2 matches what an SDR monitor shows; sRGB's linear toe lifts the
    // blacks and washes the colors out.
    rows.push_back(Setting("HDR GAMMA", {{"2.2", {{"present_hdr_gamma", "2.2"}}},
                                         {"2.4", {{"present_hdr_gamma", "2.4"}}},
                                         {"SRGB", {{"present_hdr_gamma", "0.0"}}}}));
    // The SDR image on the left, the HDR one on the right; not saved.
    {
      MenuRow compare;
      compare.label = "HDR COMPARE";
      compare.value = [] {
        return std::string(rex::cvar::GetFlagByName("present_hdr_compare") == "true"
                               ? "SDR | HDR"
                               : "OFF");
      };
      compare.adjust = [](int) {
        const bool on = rex::cvar::GetFlagByName("present_hdr_compare") == "true";
        rex::cvar::SetFlagByName("present_hdr_compare", on ? "false" : "true");
      };
      rows.push_back(std::move(compare));
    }
    rows.push_back(Page("HDR CALIBRATION", &SettingsPages::HdrCalibration));
  }
  auto note = [this] {
    const auto present = Number(Unquote(Live("host_present_fps_limit")));
    const auto game = Number(Unquote(Live("pinyon_shift_fh1_render_fps_limit")));
    if (present && *present > 0 && game && (*game == 0 || *game > *present)) {
      return std::string("THE FRAME RATE LIMIT DROPS FRAMES THE GAME RENDERED");
    }
    if (SameValue(Live("present_hdr"), "true") && !rex::ui::vulkan::GetHdrOutputState().offered) {
      return std::string("HDR NEEDS HDR TURNED ON FOR THIS DISPLAY IN WINDOWS");
    }
    return ResolutionLine();
  };
  return std::make_unique<MenuScreen>("DISPLAY", std::move(rows), std::move(note));
}

std::unique_ptr<MenuScreen> SettingsPages::HdrCalibration() {
  // Pinyon Shift's calibration: test patterns the SDK's HDR pass draws under
  // the menu, in three steps:
  // the TV's own brightness for the blacks, then the paper white, then the
  // display's peak, which the highlights expand towards. The pattern is
  // never saved; leaving the page removes it.
  auto step = std::make_shared<int>(1);
  const auto show = [](int pattern) {
    rex::cvar::SetFlagByName("present_hdr_test_pattern", std::to_string(pattern));
  };
  show(*step);
  std::vector<MenuRow> rows;
  {
    MenuRow pattern;
    pattern.label = "STEP";
    pattern.value = [step] {
      static constexpr const char* kSteps[] = {"1 BLACK LEVEL", "2 PAPER WHITE", "3 PEAK"};
      return std::string(kSteps[*step - 1]);
    };
    pattern.adjust = [step, show](int direction) {
      *step = (*step - 1 + (direction < 0 ? 2 : 1)) % 3 + 1;
      show(*step);
    };
    rows.push_back(std::move(pattern));
  }
  {
    std::vector<Choice> paper_white;
    for (int nits = 80; nits <= 500; nits += 10) {
      paper_white.push_back({std::to_string(nits) + " NITS",
                             {{"present_hdr_paper_white_nits", std::to_string(nits)}}});
    }
    MenuRow row = Setting("PAPER WHITE", std::move(paper_white));
    row.enabled = [step] { return *step == 2; };
    rows.push_back(std::move(row));
  }
  {
    std::vector<Choice> peak;
    for (int nits = 300; nits <= 4000; nits += nits < 1500 ? 50 : 100) {
      peak.push_back(
          {std::to_string(nits) + " NITS", {{"present_hdr_peak_nits", std::to_string(nits)}}});
    }
    MenuRow row = Setting("PEAK", std::move(peak));
    row.enabled = [step] { return *step == 3; };
    rows.push_back(std::move(row));
  }
  auto note = [step] {
    switch (*step) {
      case 1:
        return std::string("TV BRIGHTNESS: FIRST SQUARE BLACK, SECOND JUST SHOWS");
      case 2:
        return std::string("THE WHITE SQUARE SHOULD LOOK LIKE WHITE PAPER");
      default:
        return std::string("RAISE THE PEAK UNTIL THE SQUARE DISAPPEARS");
    }
  };
  auto screen = std::make_unique<MenuScreen>("HDR CALIBRATION", std::move(rows), std::move(note));
  // The patterns' band (hdr_output.ps.glsl kPatternTop and kPatternBottom).
  screen->set_backdrop_gap(0.62f, 0.88f);
  screen->set_on_back([show] { show(0); });
  return screen;
}


// The output mix's gain and mute, both applied by the SDK's audio driver at
// once (Pinyon Shift's AUDIO page; mixer levels need FH2's audio XML, LP-2.8).
std::unique_ptr<MenuScreen> SettingsPages::Hud() {
  std::vector<MenuRow> rows;
  // The same switch as F2: the HUD's elements hide while it is off.
  rows.push_back(Toggle("HUD", "hud_hidden", true));
  // Size around the screen's centre, over the gameplay view only: menus, the
  // map and loading screens keep theirs (ApplyNarrowView suspends it). Pinyon
  // Shift's HUD page; applies at once.
  std::vector<Choice> scales;
  for (int percent = 80; percent <= 120; percent += 5) {
    scales.push_back({std::to_string(percent) + "%",
                      {{"fh1_hud_scale", std::to_string(percent / 100.0)}}});
  }
  rows.push_back(Setting("HUD SCALE", std::move(scales)));
  // A margin kept clear on each side, for TVs that crop the picture.
  rows.push_back(Setting("SAFE ZONE", {{"OFF", {{"fh1_hud_safe_zone", "0.0"}}},
                                       {"2.5%", {{"fh1_hud_safe_zone", "0.025"}}},
                                       {"5%", {{"fh1_hud_safe_zone", "0.05"}}},
                                       {"7.5%", {{"fh1_hud_safe_zone", "0.075"}}},
                                       {"10%", {{"fh1_hud_safe_zone", "0.1"}}}}));
  return std::make_unique<MenuScreen>("HUD", std::move(rows));
}

std::unique_ptr<MenuScreen> SettingsPages::Audio() {
  std::vector<MenuRow> rows;
  std::vector<Choice> volumes;
  for (int volume = 0; volume <= 100; volume += 10) {
    volumes.push_back({std::to_string(volume), {{"lavender_master_volume", std::to_string(volume)}}});
  }
  rows.push_back(Setting("MASTER VOLUME", std::move(volumes)));
  rows.push_back(Toggle("MUTE", "audio_mute"));
  return std::make_unique<MenuScreen>("AUDIO", std::move(rows));
}

// FH2's developer launch options, held live (src/dev_options.cpp).
std::unique_ptr<MenuScreen> SettingsPages::Gameplay() {
  std::vector<MenuRow> rows;
  rows.push_back(Toggle("SKIP VOICE-OVERS WITH A", "lavender_skip_voiceover"));
  rows.push_back(Toggle("AUTOMATIC CAR RESET", "lavender_no_auto_reset", true));
  rows.push_back(Toggle("REPLAY INTRO WHEN IDLE", "lavender_no_attract_video", true));
  rows.push_back(Toggle("PHOTO CAMERA THROUGH WALLS", "lavender_photo_camera_through_walls"));
  return std::make_unique<MenuScreen>("GAMEPLAY", std::move(rows));
}

std::unique_ptr<MenuScreen> SettingsPages::Graphics() {
  std::vector<MenuRow> rows;
  const auto preset = [](const char* scale, const char* effect, const char* msaa_off,
                         const char* fps,
                         std::vector<std::pair<std::string, std::string>> extra = {}) {
    std::vector<std::pair<std::string, std::string>> values = {
        {"draw_resolution_scale_x", scale},
        {"draw_resolution_scale_y", scale},
        {"present_effect", effect},
        {"fh1_msaa_single_sample", msaa_off},
        {"fh1_msaa_2x", "false"},
        {"host_present_fps_limit", "0"},
        {"pinyon_shift_fh1_render_fps_limit", fps},
        {"vulkan_texture_max_size", "0"}};
    for (auto& [name, value] : extra) {
      const auto it = std::find_if(values.begin(), values.end(),
                                   [&](const auto& setting) { return setting.first == name; });
      if (it != values.end()) {
        it->second = value;
      } else {
        values.emplace_back(name, value);
      }
    }
    return values;
  };
  const std::vector<std::pair<std::string, std::string>> cheapest = {
      {"anisotropic_override", "-1"}, {"swap_post_effect", "\"none\""}};
  rows.push_back(Setting("GRAPHICS PRESET",
                         {{"CONSOLE 30", preset("1", "\"bilinear\"", "false", "30")},
                          {"LOW-SPEC 60", preset("1", "\"bilinear\"", "true", "60", cheapest)},
                          {"PERFORMANCE 60", preset("1", "\"fsr\"", "false", "60")},
                          {"QUALITY 60", preset("2", "\"bilinear\"", "false", "60")}}));
  {
    std::vector<Choice> scales;
    for (int scale = 1; scale <= 4; ++scale) {
      const std::string value = std::to_string(scale);
      scales.push_back({value + "X", {{"draw_resolution_scale_x", value},
                                      {"draw_resolution_scale_y", value}}});
    }
    rows.push_back(Setting("RESOLUTION SCALE", std::move(scales)));
  }
  // The game's 4x MSAA; 2X keeps two samples of each pixel's four.
  rows.push_back(Setting("MSAA", {{"4X", {{"fh1_msaa_single_sample", "false"},
                                          {"fh1_msaa_2x", "false"}}},
                                  {"2X", {{"fh1_msaa_single_sample", "false"},
                                          {"fh1_msaa_2x", "true"}}},
                                  {"OFF", {{"fh1_msaa_single_sample", "true"},
                                           {"fh1_msaa_2x", "false"}}}}));
  // anisotropic_override holds the Xenos filter: 3, 4 and 5 are 4x, 8x and
  // 16x; -1 keeps what each texture asks for.
  rows.push_back(Setting("ANISOTROPIC FILTERING",
                         {{"GAME", {{"anisotropic_override", "-1"}}},
                          {"4X", {{"anisotropic_override", "3"}}},
                          {"8X", {{"anisotropic_override", "4"}}},
                          {"16X", {{"anisotropic_override", "5"}}}}));
  rows.push_back(Toggle("TRILINEAR FILTERING", "force_trilinear_filtering"));
  rows.push_back(Setting("TEXTURE DETAIL",
                         {{"SOFT", {{"texture_mip_lod_bias", "0.5"}}},
                          {"DEFAULT", {{"texture_mip_lod_bias", "0.0"}}},
                          {"SHARP", {{"texture_mip_lod_bias", "-0.5"}}},
                          {"SHARPEST", {{"texture_mip_lod_bias", "-1.0"}}}}));
  // Larger mipmapped textures start at the first level that fits.
  rows.push_back(Setting("TEXTURE SIZE", {{"FULL", {{"vulkan_texture_max_size", "0"}}},
                                         {"1024", {{"vulkan_texture_max_size", "1024"}}},
                                         {"512", {{"vulkan_texture_max_size", "512"}}},
                                         {"256", {{"vulkan_texture_max_size", "256"}}}}));
  rows.push_back(Setting("POST-PROCESS AA",
                         {{"OFF", {{"swap_post_effect", "\"none\""}}},
                          {"FXAA", {{"swap_post_effect", "\"fxaa\""}}},
                          {"FXAA EXTREME", {{"swap_post_effect", "\"fxaa_extreme\""}}}}));
  rows.push_back(Toggle("AA ON THE HUD", "swap_post_effect_scene_only", true));
  // FH2's own post effects, switched in its code (src/graphics_hooks.cpp).
  rows.push_back(Toggle("BLOOM", "disable_bloom", true));
  rows.push_back(Toggle("MOTION BLUR", "disable_motion_blur", true));
  rows.push_back(Toggle("DEPTH OF FIELD", "disable_depth_of_field", true));
  // Through FH2's render scenarios (src/render_settings.cpp).
  rows.push_back(Toggle("SHADOWS", "lavender_shadows"));
  rows.push_back(Setting("REFLECTION UPDATES", {{"FULL", {{"lavender_env_map_rate", "1.0"}}},
                                                {"HALF", {{"lavender_env_map_rate", "0.5"}}},
                                                {"QUARTER", {{"lavender_env_map_rate", "0.25"}}}}));
  auto note = [this] { return ResolutionLine(); };
  return std::make_unique<MenuScreen>("GRAPHICS", std::move(rows), std::move(note));
}

std::unique_ptr<MenuScreen> SettingsPages::Controls() {
  std::vector<MenuRow> rows;
  MenuRow controller;
  controller.label = "CONTROLLER BUTTONS";
  controller.activate = [this] { host_ui_.Push(ControllerButtons()); };
  rows.push_back(std::move(controller));
  MenuRow map;
  map.label = "MAP CONTROLLER";
  map.activate = [this] { host_ui_.Push(MapController()); };
  rows.push_back(std::move(map));
  rows.push_back(Page("RACING WHEEL", &SettingsPages::Wheel));
  rows.push_back(Setting("RUMBLE",
                         {{"OFF", {{"pad_rumble_strength", "0"}}},
                          {"25%", {{"pad_rumble_strength", "25"}}},
                          {"50%", {{"pad_rumble_strength", "50"}}},
                          {"75%", {{"pad_rumble_strength", "75"}}},
                          {"100%", {{"pad_rumble_strength", "100"}}}}));
  rows.push_back(Toggle("MOUSE AND KEYBOARD", "mnk_mode"));
  rows.push_back(Setting("MOUSE",
                         {{"OFF", {{"mnk_mouse", "false"}, {"mnk_mouse_steering", "false"}}},
                          {"CAMERA", {{"mnk_mouse", "true"}, {"mnk_mouse_steering", "false"}}},
                          {"STEERING", {{"mnk_mouse", "false"}, {"mnk_mouse_steering", "true"}}}}));
  std::vector<Choice> sensitivities;
  for (const char* value : {"0.25", "0.5", "0.75", "1.0", "1.5", "2.0", "3.0"}) {
    sensitivities.push_back({value, {{"mnk_sensitivity", value}}});
  }
  rows.push_back(Setting("MOUSE SENSITIVITY", std::move(sensitivities)));
  // The keys each pad control maps to in mouse-and-keyboard mode. Choosing a row asks for a key; it replaces the row's keys and
  // applies at once, since the keyboard driver reads them on every press.
  static constexpr std::pair<const char*, const char*> binds[] = {
      // Labelled with the pad control and its role while driving.
      {"THROTTLE (RT)", "keybind_right_trigger"},
      {"BRAKE (LT)", "keybind_left_trigger"},
      {"STEER LEFT", "keybind_lstick_left"},
      {"STEER RIGHT", "keybind_lstick_right"},
      {"HANDBRAKE, ACCEPT (A)", "keybind_a"},
      {"SHIFT UP, BACK (B)", "keybind_b"},
      {"SHIFT DOWN (X)", "keybind_x"},
      {"REWIND (Y)", "keybind_y"},
      {"LB", "keybind_left_shoulder"},
      {"RB", "keybind_right_shoulder"},
      {"RIGHT STICK PRESS", "keybind_rstick_press"},
      {"BACK", "keybind_back"},
      {"PAUSE (START)", "keybind_start"},
  };
  for (const auto& [label, name] : binds) {
    MenuRow row;
    row.label = label;
    row.value = [this, name = std::string(name)] {
      std::string keys = Upper(Saved(name));
      for (size_t at = keys.find(','); at != std::string::npos; at = keys.find(',', at + 3)) {
        keys.replace(at, 1, " / ");
      }
      return keys.empty() ? std::string("NONE") : keys;
    };
    row.activate = [self = shared_from_this(), label = std::string(label),
                    name = std::string(name)] {
      auto prompt = std::make_unique<MenuScreen>("PRESS A KEY", std::vector<MenuRow>{});
      prompt->set_body("Press the key or mouse button for " + label +
                       ". Escape or the controller's B cancels.");
      const MenuScreen* screen = prompt.get();
      prompt->set_key_capture([self, screen, name](int virtual_key) {
        const auto key = rex::ui::VirtualKey(virtual_key);
        if (key == rex::ui::VirtualKey::kEscape) {
          self->host_ui_.Finish(screen);
          return;
        }
        const std::string key_name = rex::ui::VirtualKeyToString(key);
        if (key_name.empty()) {
          return;  // a key the keyboard driver cannot name; keep waiting
        }
        self->config_.Set(name, config::Quote(key_name));
        rex::cvar::SetFlagByName(name, key_name);
        self->Save();
        self->host_ui_.Finish(screen);
      });
      self->host_ui_.Push(std::move(prompt));
    };
    rows.push_back(std::move(row));
  }
  MenuRow reset;
  reset.label = "RESET KEYS";
  // Every keyboard bind, not only the rows above, back to the
  // driving-first keys (config/default_keys.h).
  reset.activate = [this] {
    for (const auto& key : config::kDefaultKeys) {
      config_.Set(key.name, config::Quote(key.keys));
      rex::cvar::SetFlagByName(key.name, key.keys);
    }
    Save();
  };
  rows.push_back(std::move(reset));
  return std::make_unique<MenuScreen>("CONTROLS", std::move(rows));
}

std::unique_ptr<MenuScreen> SettingsPages::ControllerButtons() {
  // One row per physical control, showing what the title receives from it;
  // swapping A and B is A SENDS B and B SENDS A. Menus here keep the
  // physical layout whatever is set.
  using rex::input::PadControl;
  std::vector<MenuRow> rows;
  for (size_t i = 0; i < rex::input::kPadControlCount; ++i) {
    MenuRow row;
    row.label = std::string(rex::input::PadControlName(PadControl(i))) + " SENDS";
    row.value = [this, i] {
      const auto remap = rex::input::ParsePadRemap(Unquote(Saved("pad_remap")));
      return std::string(rex::input::PadControlName(remap[i]));
    };
    row.adjust = [this, i](int direction) {
      auto remap = rex::input::ParsePadRemap(Unquote(Saved("pad_remap")));
      const size_t count = rex::input::kPadControlCount;
      remap[i] = PadControl((size_t(remap[i]) + count + (direction > 0 ? 1 : count - 1)) % count);
      const std::string text = rex::input::FormatPadRemap(remap);
      config_.Set("pad_remap", config::Quote(text));
      rex::cvar::SetFlagByName("pad_remap", text);
      Save();
    };
    rows.push_back(std::move(row));
  }
  rows.push_back(Toggle("INVERT LOOK", "pad_invert_right_stick_y"));
  // Buttons held together that toggle the performance panel (F3 on a
  // keyboard); the game does not see them while they are held.
  rows.push_back(Setting("PERFORMANCE PANEL",
                         {{"LS + RS", {{"pad_chord_debug_overlay", "\"LS+RS\""}}},
                          {"BACK + RS", {{"pad_chord_debug_overlay", "\"BACK+RS\""}}},
                          {"OFF", {{"pad_chord_debug_overlay", "\"\""}}}}));
  // Opens these settings from a controller alone (Steam Deck, handhelds).
  rows.push_back(Setting("SETTINGS MENU",
                         {{"BACK + START", {{"pad_chord_game_menu", "\"BACK+START\""}}},
                          {"LS + RS + START", {{"pad_chord_game_menu", "\"LS+RS+START\""}}},
                          {"OFF", {{"pad_chord_game_menu", "\"\""}}}}));
  MenuRow reset;
  reset.label = "RESET TO DEFAULT";
  reset.activate = [this] {
    config_.Set("pad_remap", config::Quote(""));
    rex::cvar::SetFlagByName("pad_remap", "");
    Save();
  };
  rows.push_back(std::move(reset));
  return std::make_unique<MenuScreen>("CONTROLLER", std::move(rows), [this] {
    return rex::input::FormatPadRemap(rex::input::ParsePadRemap(Unquote(Saved("pad_remap"))))
                   .empty()
               ? std::string("EVERY BUTTON SENDS ITSELF")
               : std::string("CHANGES APPLY AT ONCE; MENUS HERE KEEP THE PHYSICAL LAYOUT");
  });
}

std::unique_ptr<MenuScreen> SettingsPages::MapController() {
  std::vector<MenuRow> rows;
  for (auto& joystick : rex::input::sdl::ListJoysticks()) {
    MenuRow row;
    row.label = Upper(joystick.name);
    row.value = [user = joystick.user_mapping, mapped = !joystick.mapping.empty()] {
      return std::string(user ? "YOUR MAPPING" : mapped ? "MAPPED" : "NOT MAPPED");
    };
    row.activate = [self = shared_from_this(), joystick] {
      self->host_ui_.Push(self->MapControllerOptions(joystick));
    };
    rows.push_back(std::move(row));
  }
  auto screen = std::make_unique<MenuScreen>("MAP CONTROLLER", std::move(rows));
  screen->set_body(screen->rows().empty()
                       ? "No controller is connected. Connect one and open this page again."
                       : "Choose a controller. The keyboard works here too if the controller "
                         "does not move through menus yet.");
  return screen;
}

std::unique_ptr<MenuScreen> SettingsPages::MapControllerOptions(
    rex::input::sdl::JoystickInfo joystick) {
  auto user_mapping = std::make_shared<bool>(joystick.user_mapping);
  auto screen_ref = std::make_shared<MenuScreen*>(nullptr);
  std::vector<MenuRow> rows;
  MenuRow map;
  map.label = "MAP BUTTONS";
  map.activate = [self = shared_from_this(), joystick] {
    self->host_ui_.Push(self->MapControllerSteps(joystick));
  };
  rows.push_back(std::move(map));
  MenuRow reset;
  reset.label = "RESET TO DEFAULT MAPPING";
  reset.enabled = [user_mapping] { return *user_mapping; };
  reset.activate = [user_mapping, screen_ref, guid = joystick.guid] {
    *user_mapping = false;
    (*screen_ref)
        ->set_body(rex::input::sdl::RemoveUserMapping(guid)
                       ? "Your mapping is removed and the default one applies."
                       : "Your mapping is removed. The default one applies after a restart.");
  };
  rows.push_back(std::move(reset));
  // A wheel SDL does not know as one: its steering and pedals are reshaped by
  // the RACING WHEEL page once it is marked here.
  MenuRow wheel;
  wheel.label = "RACING WHEEL";
  wheel.value = [guid = joystick.guid, known = joystick.wheel_type] {
    if (known) return std::string("YES (DETECTED)");
    return std::string(rex::input::sdl::IsListedWheel(guid) ? "YES" : "NO");
  };
  wheel.enabled = [known = joystick.wheel_type] { return !known; };
  wheel.adjust = [this, guid = joystick.guid](int) {
    rex::input::sdl::SetListedWheel(guid, !rex::input::sdl::IsListedWheel(guid));
    config_.Set("hid_wheel_guids", config::Quote(rex::cvar::GetFlagByName("hid_wheel_guids")));
    diagnostics::RecordEvent("settings.changed",
                             {{"row", "RACING WHEEL"},
                              {"values", "hid_wheel_guids=" +
                                             rex::cvar::GetFlagByName("hid_wheel_guids")}});
    Save();
  };
  wheel.activate = [adjust = wheel.adjust] { adjust(1); };
  rows.push_back(std::move(wheel));
  auto screen = std::make_unique<MenuScreen>(Upper(joystick.name), std::move(rows));
  *screen_ref = screen.get();
  screen->set_body(joystick.user_mapping ? "This controller uses a mapping you made."
                   : joystick.mapping.empty()
                       ? "SDL does not know this controller. Map its buttons to use it."
                       : "This controller uses SDL's mapping for it.");
  return screen;
}

std::unique_ptr<MenuScreen> SettingsPages::MapControllerSteps(
    rex::input::sdl::JoystickInfo joystick) {
  struct Step {
    const char* prompt;
    const char* output;
    // The same control's name on a PlayStation pad, or null.
    const char* playstation = nullptr;
  };
  // SDL's names for the Xbox 360 controls; a leading sign is a half axis.
  static constexpr Step kSteps[] = {
      {"A", "a", "CROSS"},
      {"B", "b", "CIRCLE"},
      {"X", "x", "SQUARE"},
      {"Y", "y", "TRIANGLE"},
      {"LEFT BUMPER (LB)", "leftshoulder", "L1"},
      {"RIGHT BUMPER (RB)", "rightshoulder", "R1"},
      {"LEFT TRIGGER (LT)", "lefttrigger", "L2"},
      {"RIGHT TRIGGER (RT)", "righttrigger", "R2"},
      {"BACK", "back", "CREATE (SHARE)"},
      {"START", "start", "OPTIONS"},
      {"GUIDE (XBOX BUTTON)", "guide", "PS BUTTON"},
      {"D-PAD UP", "dpup"},
      {"D-PAD DOWN", "dpdown"},
      {"D-PAD LEFT", "dpleft"},
      {"D-PAD RIGHT", "dpright"},
      {"LEFT STICK LEFT", "-leftx"},
      {"LEFT STICK RIGHT", "+leftx"},
      {"LEFT STICK UP", "-lefty"},
      {"LEFT STICK DOWN", "+lefty"},
      {"LEFT STICK PRESS", "leftstick", "L3 (LEFT STICK PRESS)"},
      {"RIGHT STICK LEFT", "-rightx"},
      {"RIGHT STICK RIGHT", "+rightx"},
      {"RIGHT STICK UP", "-righty"},
      {"RIGHT STICK DOWN", "+righty"},
      {"RIGHT STICK PRESS", "rightstick", "R3 (RIGHT STICK PRESS)"},
  };
  static constexpr size_t kStepCount = std::size(kSteps);
  static constexpr auto kStepTime = std::chrono::seconds(8);
  static constexpr auto kHoldTime = std::chrono::seconds(2);
  enum class Phase { kCapture, kTest, kDone };
  struct Session {
    rex::input::sdl::JoystickInfo joystick;
    rex::input::sdl::JoystickCapture capture;
    rex::input::sdl::GamepadView view;
    // The captured input for each step, empty when skipped.
    std::vector<std::string> inputs = std::vector<std::string>(kStepCount);
    size_t step = 0;
    Phase phase = Phase::kCapture;
    std::string note;
    std::chrono::steady_clock::time_point step_started, phase_started;
    std::string held_control;
    std::chrono::steady_clock::time_point held_since;
    bool saved = false;
  };
  auto session = std::make_shared<Session>();
  session->joystick = std::move(joystick);
  const bool opened = session->capture.Begin(session->joystick.instance_id);
  session->step_started = std::chrono::steady_clock::now();

  // Prompts in the labels printed on the pad being mapped.
  const bool playstation = session->joystick.playstation;
  const auto prompt = [playstation](const Step& step) {
    return std::string(playstation && step.playstation ? step.playstation : step.prompt);
  };
  const auto control_name = [prompt](const std::string& output) {
    for (const Step& step : kSteps) {
      if (output == step.output) return prompt(step);
    }
    return Upper(output);
  };
  // Pairs of half axes on one input become a full axis ("leftx:a0").
  const auto mapping = [session] {
    std::vector<std::pair<std::string, std::string>> bindings;
    for (size_t i = 0; i < kStepCount; ++i) {
      const std::string output = kSteps[i].output;
      const std::string& input = session->inputs[i];
      if (input.empty()) continue;
      if (output[0] == '-' || output[0] == '+') {
        size_t other = 0;
        while (other < kStepCount &&
               (other == i ||
                std::string_view(kSteps[other].output).substr(1) != output.substr(1))) {
          ++other;
        }
        if (other < kStepCount) {
          const std::string& paired = session->inputs[other];
          if (input.size() > 1 && paired.size() > 1 && (input[0] == '+' || input[0] == '-') &&
              input.substr(1) == paired.substr(1) && input[0] != paired[0]) {
            if (output[0] == '+') {
              bindings.emplace_back(output.substr(1),
                                    input[0] == '+' ? input.substr(1) : input.substr(1) + "~");
            }
            continue;
          }
        }
      }
      bindings.emplace_back(output, input);
    }
    return rex::input::sdl::BuildMapping(session->joystick, bindings);
  };
  // Leaving or starting over while trying a mapping puts the old one back.
  const auto restore = [session] {
    if (session->phase == Phase::kTest && !session->joystick.mapping.empty()) {
      rex::input::sdl::ApplyMapping(session->joystick.mapping);
    }
  };
  const auto begin_test = [session, mapping] {
    session->capture.End();
    session->phase = Phase::kTest;
    session->phase_started = std::chrono::steady_clock::now();
    session->held_control.clear();
    if (!rex::input::sdl::ApplyMapping(mapping()) ||
        !session->view.Begin(session->joystick.instance_id)) {
      session->note = "The mapping could not be applied (see the log).";
    }
  };
  const auto restart = [session, restore] {
    restore();
    session->view.End();
    session->inputs.assign(kStepCount, std::string());
    session->step = 0;
    session->phase = Phase::kCapture;
    session->note.clear();
    session->capture.Begin(session->joystick.instance_id);
    session->step_started = std::chrono::steady_clock::now();
  };
  const auto save = [session, mapping] {
    session->view.End();
    session->saved = rex::input::sdl::SaveMapping(mapping());
    session->phase = Phase::kDone;
    session->phase_started = std::chrono::steady_clock::now();
  };
  const auto advance = [session, begin_test] {
    session->note.clear();
    if (++session->step >= kStepCount) {
      session->step = kStepCount - 1;
      begin_test();
    }
    session->step_started = std::chrono::steady_clock::now();
  };

  auto screen = std::make_unique<MenuScreen>(Upper(session->joystick.name), std::vector<MenuRow>{});
  MenuScreen* raw = screen.get();
  const auto show = [session, raw, control_name, prompt] {
    const auto now = std::chrono::steady_clock::now();
    const std::string note = session->note.empty() ? std::string() : session->note + "\n";
    std::string body;
    switch (session->phase) {
      case Phase::kCapture: {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            kStepTime - (now - session->step_started));
        body = "Press " + prompt(kSteps[session->step]) + " on the controller.\n" +
               std::to_string(session->step + 1) + " of " + std::to_string(kStepCount) +
               ". Skipped in " + std::to_string(std::max<int64_t>(0, left.count() + 1)) +
               " s if the controller has none.\n" + note +
               "Keyboard: any key skips, Backspace goes back, R starts over, Escape cancels.";
        break;
      }
      case Phase::kTest: {
        std::string held;
        for (const auto& control : session->view.Held()) {
          held += (held.empty() ? "" : ", ") + control_name(control);
        }
        body = "Try the controller. Held: " + (held.empty() ? std::string("nothing") : held) +
               "\n" + note +
               "Hold START for 2 s to save it, or BACK to map it again.\n"
               "Keyboard: Enter saves, R maps again, Escape cancels.";
        break;
      }
      case Phase::kDone:
        body = session->saved ? "Saved. The controller now works as an Xbox 360 controller."
                              : "The mapping could not be saved (see the log).";
        break;
    }
    raw->set_body(std::move(body));
  };
  if (!opened) {
    session->phase = Phase::kDone;
    raw->set_body("The controller could not be opened. Press any key to go back.");
  } else {
    show();
    screen->set_tick([self = shared_from_this(), session, raw, show, advance, restart, save, prompt] {
      const auto now = std::chrono::steady_clock::now();
      switch (session->phase) {
        case Phase::kCapture:
          if (auto input = session->capture.Poll()) {
            const auto used = std::find(session->inputs.begin(), session->inputs.end(), *input);
            if (used == session->inputs.end()) {
              session->inputs[session->step] = *input;
              advance();
            } else {
              session->note = "That is already " +
                              prompt(kSteps[used - session->inputs.begin()]) +
                              ". Press another control.";
            }
          } else if (now - session->step_started >= kStepTime) {
            advance();
          }
          break;
        case Phase::kTest: {
          // Holding START or BACK (through the new mapping) decides, so the
          // controller alone can finish.
          const auto held = session->view.Held();
          std::string control;
          for (const char* name : {"start", "back"}) {
            if (std::find(held.begin(), held.end(), name) != held.end()) control = name;
          }
          if (control != session->held_control) {
            session->held_control = control;
            session->held_since = now;
          } else if (!control.empty() && now - session->held_since >= kHoldTime) {
            if (control == "start") {
              save();
            } else {
              restart();
            }
          }
          break;
        }
        case Phase::kDone:
          // The result stays up for a few seconds, then the screen goes back
          // by itself, so a player with only the controller is not stuck.
          if (now - session->phase_started >= std::chrono::seconds(4)) {
            self->host_ui_.Finish(raw);
            return;
          }
          break;
      }
      show();
    });
  }
  screen->set_key_capture(
      [self = shared_from_this(), session, raw, show, advance, restart, save, restore](
          int virtual_key) {
        using rex::ui::VirtualKey;
        const auto key = VirtualKey(virtual_key);
        if (session->phase == Phase::kDone || key == VirtualKey::kEscape) {
          restore();
          session->capture.End();
          session->view.End();
          self->host_ui_.Finish(raw);
          return;
        }
        if (key == VirtualKey::kR) {
          restart();
        } else if (session->phase == Phase::kTest) {
          if (key == VirtualKey::kReturn) save();
        } else if (key == VirtualKey::kBack) {
          if (session->step > 0) {
            session->inputs[--session->step].clear();
            session->note.clear();
            session->step_started = std::chrono::steady_clock::now();
          }
        } else {
          advance();
        }
        show();
      });
  screen->set_on_back([session, restore] {
    restore();
    session->capture.End();
    session->view.End();
  });
  return screen;
}


std::unique_ptr<MenuScreen> SettingsPages::Wheel() {
  std::vector<MenuRow> rows;
  const auto percents = [](const char* name, std::initializer_list<int> values) {
    std::vector<Choice> choices;
    for (int value : values) {
      choices.push_back({std::to_string(value) + "%", {{name, std::to_string(value)}}});
    }
    return choices;
  };
  // Full lock at this share of the wheel's rotation: 50% on a 900-degree
  // wheel locks at 225 degrees each side.
  rows.push_back(Setting("STEERING SATURATION",
                         percents("hid_wheel_steering_saturation",
                                  {100, 90, 80, 70, 60, 50, 40, 30, 20})));
  // Skips the game's stick deadzone so the car answers the first degrees.
  rows.push_back(Setting("STEERING DEADZONE",
                         percents("hid_wheel_steering_deadzone", {0, 5, 10, 15, 20, 25, 30})));
  std::vector<Choice> curves;
  for (const auto& [label, value] : std::initializer_list<std::pair<const char*, int>>{
           {"LINEAR", 100}, {"SOFT CENTRE", 150}, {"SOFTER CENTRE", 200}, {"SHARP CENTRE", 75}}) {
    curves.push_back({label, {{"hid_wheel_steering_linearity", std::to_string(value)}}});
  }
  rows.push_back(Setting("STEERING CURVE", std::move(curves)));
  rows.push_back(
      Setting("PEDAL DEADZONE", percents("hid_wheel_pedal_deadzone", {0, 5, 10, 15, 20})));
  MenuRow map;
  map.label = "MAP A WHEEL";
  map.activate = [this] { host_ui_.Push(MapController()); };
  rows.push_back(std::move(map));
  auto screen = std::make_unique<MenuScreen>("RACING WHEEL", std::move(rows), [] {
    int wheels = 0;
    for (const auto& joystick : rex::input::sdl::ListJoysticks()) {
      if (joystick.wheel_type || rex::input::sdl::IsListedWheel(joystick.guid)) ++wheels;
    }
    return wheels == 0   ? std::string("NO WHEEL CONNECTED")
           : wheels == 1 ? std::string("1 WHEEL CONNECTED")
                         : std::to_string(wheels) + " WHEELS CONNECTED";
  });
  screen->set_body(
      "A wheel drives through its controller mapping: steering on the left stick, throttle on "
      "the right trigger and brake on the left trigger. If the game does not see it, choose MAP "
      "A WHEEL, then the wheel, and mark it as a racing wheel.");
  return screen;
}

// Pinyon Shift's PROFILE page without GAMERTAG and LANGUAGE: both apply only
// at the next start, against this menu's rule, and FH2's language codes are
// not checked.
std::unique_ptr<MenuScreen> SettingsPages::Profile() {
  std::vector<MenuRow> rows;
  MenuRow backups;
  backups.label = "SAVE BACKUPS";
  backups.activate = [this] { host_ui_.Push(Backups()); };
  rows.push_back(std::move(backups));
  return std::make_unique<MenuScreen>("PROFILE", std::move(rows));
}

// "20260929T074240Z-session" as "2026-09-29 07:42 SESSION START".
std::string SlotLabel(const std::string& name) {
  if (name.size() < 16) {
    return Upper(name);
  }
  std::string label = name.substr(0, 4) + "-" + name.substr(4, 2) + "-" + name.substr(6, 2) +
                      " " + name.substr(9, 2) + ":" + name.substr(11, 2);
  const std::string reason = name.size() > 17 ? name.substr(17) : std::string();
  if (reason == "session") {
    label += " SESSION START";
  } else if (reason == "before-restore") {
    label += " BEFORE RESTORE";
  } else if (reason == "exit") {
    label += " EXIT";
  }
  return label;
}

std::unique_ptr<MenuScreen> SettingsPages::Backups() {
  SaveBackups* backups = services_.save_backups;
  std::vector<MenuRow> rows;
  if (backups->RestorePending()) {
    MenuRow cancel;
    cancel.label = "CANCEL THE RESTORE";
    cancel.activate = [backups] { backups->CancelRestore(); };
    rows.push_back(std::move(cancel));
  }
  for (const auto& slot : backups->List()) {
    MenuRow row;
    row.label = SlotLabel(slot.name);
    row.value = [kilobytes = slot.bytes >> 10] { return std::to_string(kilobytes) + " KB"; };
    row.activate = [this, name = slot.name] { host_ui_.Push(ConfirmRestore(name)); };
    rows.push_back(std::move(row));
  }
  const bool empty = rows.empty();
  return std::make_unique<MenuScreen>("SAVE BACKUPS", std::move(rows), [backups, empty] {
    if (backups->RestorePending()) {
      return std::string("RESTART THE GAME TO RESTORE THE CHOSEN BACKUP");
    }
    return std::string(empty ? "A BACKUP IS TAKEN AFTER THE GAME SAVES" : "NEWEST FIRST, UTC TIMES");
  });
}

std::unique_ptr<MenuScreen> SettingsPages::ConfirmRestore(std::string slot) {
  SaveBackups* backups = services_.save_backups;
  auto self = std::make_shared<const MenuScreen*>(nullptr);
  std::vector<MenuRow> rows(2);
  rows[0].label = "RESTORE AT NEXT START";
  rows[0].activate = [this, backups, slot, self] {
    backups->ScheduleRestore(slot);
    host_ui_.Finish(*self);
  };
  rows[1].label = "CANCEL";
  rows[1].activate = [this, self] { host_ui_.Finish(*self); };
  auto screen = std::make_unique<MenuScreen>("RESTORE BACKUP", std::move(rows));
  screen->set_body("The game will restore the saves from " + SlotLabel(slot) +
                   " when it next starts. Your current saves are backed up first, so this can "
                   "be undone.");
  screen->SetFocus(1);
  *self = screen.get();
  return screen;
}

std::unique_ptr<MenuScreen> SettingsPages::Root() {
  std::vector<MenuRow> rows;
  MenuRow resume;
  resume.label = "RESUME";
  resume.activate = [self = shared_from_this()] { self->host_ui_.Close(); };
  rows.push_back(std::move(resume));
  rows.push_back(Page("DISPLAY", &SettingsPages::Display));
  rows.push_back(Page("GRAPHICS", &SettingsPages::Graphics));
  rows.push_back(Page("AUDIO", &SettingsPages::Audio));
  rows.push_back(Page("CONTROLS", &SettingsPages::Controls));
  rows.push_back(Page("GAMEPLAY", &SettingsPages::Gameplay));
  if (services_.save_backups) rows.push_back(Page("PROFILE", &SettingsPages::Profile));
  if (services_.quit) {
    MenuRow quit;
    quit.label = "QUIT GAME";
    quit.activate = [self = shared_from_this()] {
      self->host_ui_.Close();
      self->services_.quit();
    };
    rows.push_back(std::move(quit));
  }
  return std::make_unique<MenuScreen>("SETTINGS", std::move(rows));
}

}  // namespace

std::unique_ptr<hostui::MenuScreen> CreateSettingsMenu(hostui::HostUi& host_ui,
                                                       config::HostConfig& config,
                                                       SettingsServices services) {
  if (!config.Load()) {
    REXLOG_ERROR("Settings: cannot read {}; changes will not be saved",
                 rex::path_to_utf8(config.path()));
  }
  return std::make_shared<SettingsPages>(host_ui, config, std::move(services))->Root();
}

}  // namespace lavender_shift::ui
