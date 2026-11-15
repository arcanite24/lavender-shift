// FH2's D3D layer waits for ring-buffer space by calling a poll function
// (TU v2 sub_82B72EA8; base disc sub_82B72A90) in a loop. Each call spins
// briefly, re-reads the read pointer the command processor writes back and
// returns 1 to keep waiting or 0 to give up (device lost, or its hang check).
// On the host that loop keeps the render thread at a full core while the CP
// works through the ring.
//
// Whenever the read pointer has not moved since the last poll, the override
// can first re-read it for a few microseconds (lavender_ring_wait_spin_us;
// Pinyon Shift spins 20 us in its fence wait, but four desktop runs here
// could not tell 0 from 20 apart, so it stays 0), then blocks until the CP
// writes something CPU-visible (the read pointer included, with
// gpu_read_pointer_publish_dwords set) or the timeout passes. The original
// then runs unchanged, so its bookkeeping and hang check
// still see real time pass.

#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/gpu_write_signal.h>

REXCVAR_DEFINE_INT32(lavender_ring_wait_us, 1000, "Lavender Shift",
                     "Longest sleep, in microseconds, in each poll of FH2's wait for "
                     "ring-buffer space while the read pointer has not moved; 0 keeps "
                     "the game's busy poll")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(lavender_ring_wait_spin_us, 0, "Lavender Shift",
                     "Microseconds each poll of FH2's ring-buffer wait re-reads the read "
                     "pointer before it sleeps (lavender_ring_wait_us)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" REX_FUNC(__imp__sub_82B72EA8);

namespace {

// Guest loads. Windows maps every guest address at base + address; the
// 0xE0000000 physical view needs a host offset on macOS and some Linux
// kernels, which this desktop-only build does not handle yet.
uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

// The same, as an atomic load the compiler re-reads in a polling loop.
uint32_t LoadAcquire32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(
      __atomic_load_n(reinterpret_cast<const uint32_t*>(base + address), __ATOMIC_ACQUIRE));
}

// Offsets into the wait state (r3) and the device it points to.
constexpr uint32_t kStateDevice = 0x0;
constexpr uint32_t kStateLastReadPointer = 0x8;
constexpr uint32_t kDeviceReadPointerAddress = 0x2B10;

}  // namespace

extern "C" REX_FUNC(sub_82B72EA8) {
  const int32_t wait_us = REXCVAR_GET(lavender_ring_wait_us);
  if (wait_us > 0) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t device = Load32(base, state + kStateDevice);
    const uint32_t read_pointer_address = Load32(base, device + kDeviceReadPointerAddress);
    // Take the sequence before reading the pointer, so a writeback in
    // between ends the wait at once.
    const uint32_t seen = rex::system::GpuWriteSequence();
    const uint32_t last_seen = Load32(base, state + kStateLastReadPointer);
    if (read_pointer_address && LoadAcquire32(base, read_pointer_address) == last_seen) {
      bool moved = false;
      if (const int32_t spin_us = REXCVAR_GET(lavender_ring_wait_spin_us); spin_us > 0) {
        const auto spin_end = std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
        do {
          moved = LoadAcquire32(base, read_pointer_address) != last_seen;
        } while (!moved && std::chrono::steady_clock::now() < spin_end);
      }
      if (!moved) rex::system::WaitForGpuWrite(seen, std::chrono::microseconds(wait_us));
    }
  }
  __imp__sub_82B72EA8(ctx, base);
}
