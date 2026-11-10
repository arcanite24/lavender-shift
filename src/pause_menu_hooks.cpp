// The pause menu's ONLINE ROAD TRIP tile opens the host settings menu
// (docs/PINYON_PARITY.md LP-2.11). Offline the tile only shows FEATURE
// UNAVAILABLE, as FH1's MULTIPLAYER row did, which Pinyon Shift turned into
// SETTINGS the same way. Pinyon's FH1 hooks do not map to FH2, whose pause
// menu is a page of tiles (CAnthem_PauseMenu).
//
// A tile that needs Xbox Live, offline, calls sub_826CDF40(owner, request)
// through a delegate (sub_826D1EE8). The function picks the reason's text,
// posts it as UI message 251 (FEATURE UNAVAILABLE) and returns true. The
// request's second word names the feature: 2 for the Home page's ONLINE
// ROAD TRIP, 9 for the Social page's FRIENDS. Found by logging the callers
// of the UI message post (sub_826A3510) while pressing each tile; the
// pause menu's own locked-tile popups (sub_826F60F8, sub_826F6D28) are not
// on this path.
//
// The tile's label is IDS_MultiplayerOption from the pause menu's string
// table ("AN_PauseMenu"); its ID is held in the global 0x834584E4, filled
// at startup by name, and only the online tile's builders load it. The
// table lookup, sub_82E34E90(table, id), returns the text as big-endian
// UTF-16; for that ID it returns "PC SETTINGS" instead.

#include "pause_menu_hooks.h"

#include <mutex>
#include <string_view>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(lavender_pause_settings, true, "Lavender Shift",
                    "The pause menu's ONLINE ROAD TRIP tile opens the settings menu");

namespace {

constexpr uint32_t kOnlineRoadTrip = 2;
constexpr uint32_t kMultiplayerOptionId = 0x834584E4;
constexpr std::u16string_view kLabel = u"PC SETTINGS";

std::mutex g_label_mutex;
uint32_t g_label = 0;

// The label in guest memory, made on first use.
uint32_t Label() {
  std::lock_guard lock(g_label_mutex);
  if (g_label) return g_label;
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state) return 0;
  const uint32_t size = uint32_t((kLabel.size() + 1) * 2);
  const uint32_t label = kernel_state->memory()->SystemHeapAlloc(size);
  if (!label) return 0;
  uint8_t* text = lavender_shift::guest::Pointer(label);
  for (size_t i = 0; i <= kLabel.size(); ++i) {
    const char16_t c = i < kLabel.size() ? kLabel[i] : u'\0';
    text[i * 2] = uint8_t(c >> 8);
    text[i * 2 + 1] = uint8_t(c);
  }
  g_label = label;
  return label;
}

std::mutex g_mutex;
std::function<void()> g_handler;

bool OpenSettings() {
  std::function<void()> handler;
  {
    std::lock_guard lock(g_mutex);
    handler = g_handler;
  }
  if (!REXCVAR_GET(lavender_pause_settings) || !handler) return false;
  REXLOG_INFO("Pause menu: ONLINE ROAD TRIP opens the settings");
  handler();
  return true;
}

}  // namespace

namespace lavender_shift::pause_menu {

void SetSettingsHandler(std::function<void()> handler) {
  std::lock_guard lock(g_mutex);
  g_handler = std::move(handler);
}

}  // namespace lavender_shift::pause_menu

// At 0x826CDF58, after sub_826CDF40's frame is set up (r3 the owner, r4 the
// request). Returning true jumps to the epilogue, which returns the byte at
// r1+124: set to 1 (handled), so no popup is built.
bool LavenderLiveUnavailable(PPCContext& context, uint8_t* /*base*/) {
  using namespace lavender_shift::guest;
  const uint32_t request = context.r4.u32;
  if (!Readable(request + 4, 4) || LoadU32(request + 4) != kOnlineRoadTrip) return false;
  if (!OpenSettings()) return false;
  StoreU8(uint32_t(context.r1.u64) + 124, 1);
  return true;
}

// At 0x82E34EA0 in sub_82E34E90(table, id), after its frame is set up (r4
// the string ID). Returning true jumps to the epilogue with r3 the text.
bool LavenderStringLookup(PPCContext& context, uint8_t* /*base*/) {
  using namespace lavender_shift::guest;
  const uint32_t id = context.r4.u32;
  if (!id || !REXCVAR_GET(lavender_pause_settings) || id != LoadU32(kMultiplayerOptionId)) {
    return false;
  }
  const uint32_t label = Label();
  if (!label) return false;
  context.r3.u64 = label;
  return true;
}
