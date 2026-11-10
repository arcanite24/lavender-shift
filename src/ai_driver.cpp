// Diagnostic: hand the player's car to the title's own AI driver, for
// scripted test routes (docs/PINYON_PARITY.md LP-0.4), as Pinyon Shift's
// render tests do (fh1_render_test_rally_ai_driver).
//
// FH2 TU v2 addresses, mapped from FH1 v4 by tools/map-fh1-v4-addresses.py
// in Pinyon Shift and checked by their instructions:
// - 0x83422224: the game object pointer (FH1 v4 0x833CAAE4, data).
// - 0x8248F578(game, 0): returns where the world pointer is (FH1 v4
//   0x824AF1E8, aligned); the player-slot callers all read the world as
//   [0x8248F578([0x83422224], 0)]. session = [world + 124] and its mode at
//   +56 are FH1's offsets, logged here to check them on FH2.
// - 0x8249BE10(world): the player's car slot (FH1 v4 0x824BB8F8, exact).
// - 0x824FAA98(car, mode): the car's control-mode setter, 0 player, 1 AI
//   (FH1 v4 0x8250B050). Same instructions as FH1's; the car fields it reads
//   sit 160 bytes later in FH2 (control mode at +15400, FH1 v4 +15240).
//
// The step hook calls Update() on the simulation thread, so the calls run on
// a guest thread through the SDK's trap dispatch.

#include "ai_driver.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(lavender_ai_drives_player, false, "Lavender Shift",
                    "Diagnostic: let the game's AI drive the player's car (test routes)");

namespace {

constexpr uint32_t kGame = 0x83422224u;
constexpr uint32_t kWorldOf = 0x8248F578u;
constexpr uint32_t kPlayerSlot = 0x8249BE10u;
constexpr uint32_t kSetControl = 0x824FAA98u;
constexpr uint32_t kControlMode = 15400;
constexpr uint32_t kFreeRoam = 17;

uint32_t ai_car = 0;
uint32_t last_mode = 0xFFFFFFFFu;

uint32_t Pointer(uint32_t field, uint32_t size) {
  using namespace lavender_shift::guest;
  if (!Readable(field, 4)) return 0;
  const uint32_t value = LoadU32(field);
  return value && Readable(value, size) ? value : 0;
}

uint32_t Call(uint32_t address, uint32_t a0, uint32_t a1 = 0) {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->function_dispatcher()->GetFunction(address)) return 0;
  uint64_t args[6] = {a0, a1, 0, 0, 0, 0};
  return uint32_t(kernel_state->function_dispatcher()->ExecuteTrap(
      rex::runtime::ThreadState::Get(), address, args, 6));
}

}  // namespace

namespace lavender_shift::ai_driver {

uint32_t GameMode() { return last_mode; }

void Update() {
  static uint32_t steps = 0;
  if (++steps % 30) return;
  const uint32_t game = Pointer(kGame, 8);
  const uint32_t where = game ? Call(kWorldOf, game, 0) : 0;
  const uint32_t world = where ? Pointer(where, 136) : 0;
  const uint32_t session = world ? Pointer(world + 124, 60) : 0;
  const uint32_t mode = session ? guest::LoadU32(session + 56) : 0xFFFFFFFFu;
  if (mode != last_mode) {
    REXLOG_INFO("Game mode {} (game {:08X}, world at {:08X}, world {:08X}, session {:08X})",
                int32_t(mode), game, where, world, session);
    last_mode = mode;
  }
  const bool wanted = REXCVAR_GET(lavender_ai_drives_player);
  if (!wanted && !ai_car) return;
  if (!world) {
    ai_car = 0;
    return;
  }
  const uint32_t slot = Call(kPlayerSlot, world, 0);
  const uint32_t car = slot ? Pointer(slot, kControlMode + 4) : 0;
  if (!wanted) {
    if (car == ai_car) Call(kSetControl, car, 0);
    REXLOG_INFO("AI driver: car {:08X} back to the player", ai_car);
    ai_car = 0;
    return;
  }
  if (!car || car == ai_car) {
    static uint32_t misses = 0;
    if (!car && misses++ % 20 == 0) REXLOG_INFO("AI driver: no car (slot {:08X})", slot);
    return;
  }
  // In a race, only take a car the title has handed to the player (control
  // mode 0), so the AI never competes with a scripted intro.
  const uint32_t control = guest::LoadU32(car + kControlMode);
  if (control != 0 && mode != kFreeRoam) {
    static uint32_t waits = 0;
    if (waits++ % 20 == 0) REXLOG_INFO("AI driver: car {:08X} control {}", car, control);
    return;
  }
  Call(kSetControl, car, 0);
  Call(kSetControl, car, 1);
  ai_car = car;
  REXLOG_INFO("AI driver: car {:08X} handed to the AI (game mode {}, control was {}, now {})",
              car, int32_t(mode), control, guest::LoadU32(car + kControlMode));
}

}  // namespace lavender_shift::ai_driver
