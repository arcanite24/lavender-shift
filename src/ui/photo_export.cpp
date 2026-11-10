// F8 photos (docs/PINYON_PARITY.md, LP-1.11), Pinyon Shift's photo export.

#include "ui/photo_export.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "ui/png_writer.h"

namespace lavender_shift::ui {
namespace {
std::atomic<bool> g_saving{false};
}  // namespace

void SavePhoto(rex::ui::Presenter* presenter, std::filesystem::path directory) {
  if (!presenter || g_saving.exchange(true)) {
    return;  // one photo at a time; a held key must not queue a burst
  }
  std::thread([presenter, directory = std::move(directory)] {
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("Photo: no game frame to capture");
      g_saving = false;
      return;
    }
    const std::vector<uint8_t> png =
        EncodePng(image.data.data(), image.width, image.height, image.stride, 4);
    const auto now = std::chrono::system_clock::now();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto path = directory / fmt::format(
        "lavender-shift-{:%Y%m%dT%H%M%S}Z-{:03}.png", std::chrono::floor<std::chrono::seconds>(now),
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    const bool written = bool(file);
    file.close();
    if (written) {
      REXLOG_INFO("Photo saved: {} ({}x{}, {} KB)", rex::path_to_utf8(path), image.width,
                  image.height, png.size() >> 10);
    } else {
      REXLOG_ERROR("Photo: could not write {}", rex::path_to_utf8(path));
    }
    g_saving = false;
  }).detach();
}

}  // namespace lavender_shift::ui
