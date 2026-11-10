// Skipping the boot splash movies (docs/PINYON_PARITY.md, LP-2.7), and the
// guest file-open observer that tells which movie is playing.
//
// FH2 keeps its splash movies in media/ui/videos/splash_intros, as FH1 does.
// The XMedia wrapper's result passes through TU v2 0x82FEB3DC (Pinyon
// Shift's FH1 v4 0x82F1F504, an exact function match with identical
// instructions). Returning the decoder's end-of-stream result there runs the
// game's ordinary movie-finished path, so profile and setup state are what
// they would be after watching. Only splash movies are ended: ending every
// stream also ends PressStart.wmv's loop behind the title menus.

#include <atomic>
#include <cctype>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/io.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "dev_options.h"
#include "movie_hooks.h"

REXCVAR_DEFINE_BOOL(lavender_skip_intro_movies, true, "Lavender Shift",
                    "Skip the logo movies at boot (media/ui/videos/splash_intros)");

namespace {
// The decoder's end-of-stream result (the SDK's XMedia facade returns it).
constexpr uint32_t kEndOfStream = 0x16660026u;
std::atomic<bool> g_movie_is_splash{false};
std::atomic<uint32_t> g_skipped{0};

void ObserveGuestFileOpen(std::string_view guest_path) {
  // The launch-options object is read during boot, before the first step.
  lavender_shift::dev_options::Update();
  if (guest_path.size() < 4) return;
  std::string path(guest_path);
  for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (!path.ends_with(".wmv")) return;
  // Movies play one at a time, so the last one opened is the one decoding.
  g_movie_is_splash.store(path.find("splash_intros") != std::string::npos,
                          std::memory_order_release);
}
}  // namespace

void LavenderCompleteOpeningMovie(PPCRegister& r3) {
  if (!REXCVAR_GET(lavender_skip_intro_movies) ||
      !g_movie_is_splash.load(std::memory_order_acquire)) {
    return;
  }
  r3.u32 = kEndOfStream;
  if (g_skipped.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("Skipping the intro movies");
  }
}

namespace lavender_shift::movies {
void Install() { rex::kernel::xboxkrnl::SetGuestFileOpenObserver(&ObserveGuestFileOpen); }
void Uninstall() { rex::kernel::xboxkrnl::SetGuestFileOpenObserver(nullptr); }
}  // namespace lavender_shift::movies
