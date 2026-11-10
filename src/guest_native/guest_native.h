#pragma once

// Hand-written native replacements of hot recompiled guest functions, after
// Pinyon Shift's src/guest_native (docs/PINYON_PARITY.md LP-4.3).
//
// Every generated function `sub_X` is a weak alias of `__imp__sub_X`, so a
// strong `sub_X` defined here replaces it at every direct and indirect call
// while `__imp__sub_X` stays the original. LAVENDER_GUEST_NATIVE defines that
// strong symbol for the build's variant (base disc or v4) and routes each
// call through Dispatch, which:
//
// - checks once that the guest code at the address hashes to what the
//   replacement was written against, and otherwise runs the original for
//   good (a different executable or title update);
// - runs the native body when the global switch and the replacement's own
//   cvar are on; the body may decline an input (return false) and the
//   original then runs, so rare paths (errors, special values) need no port;
// - every Nth call when lavender_guest_native_verify is N, runs the
//   native body on a copy of the registers with the memory it writes saved
//   and restored, then the original for real, and compares return values,
//   non-volatile registers and the written memory. The original's results
//   are what the game keeps; mismatches are logged with their inputs.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace lavender_shift::guest_native {

// Registers a replacement returns, compared in verify mode.
enum ReturnRegisters : uint32_t {
  kReturnNone = 0,
  kReturnR3 = 1u << 0,
  kReturnF1 = 1u << 1,
  kReturnV3 = 1u << 2,
};

struct GuestRange {
  uint32_t address;
  uint32_t size;
};

constexpr size_t kMaxWrittenRanges = 4;

struct Stats {
  std::atomic<uint64_t> native{0};
  std::atomic<uint64_t> declined{0};
  std::atomic<uint64_t> original{0};
  std::atomic<uint64_t> verified{0};
  std::atomic<uint64_t> mismatched{0};
  // Time spent in the verified calls, native and original.
  std::atomic<uint64_t> native_ns{0};
  std::atomic<uint64_t> original_ns{0};
};

struct Replacement {
  // Short stable name for logs and the backlog, e.g. "crt.pow".
  const char* name;
  // Guest address in this build and the guest code checked before first use.
  uint32_t guest_address;
  uint32_t guest_code_size;
  uint64_t guest_code_fnv1a;
  uint32_t returns;
  // The replacement's own switch (its cvar).
  bool (*enabled)();
  // The native body: true when it handled the call, false to run the original.
  bool (*native)(PPCContext& ctx, uint8_t* base);
  // Guest memory the body may write for this input; null when it writes none.
  size_t (*written)(const PPCContext& ctx, uint8_t* base, GuestRange* ranges);
  // Optional verify mode for a body accepted within a tolerance instead of
  // bit for bit (NN-4.3's DSP twins): it replaces the comparison above, must
  // run `original` on the real registers and memory and leave its results, and
  // updates `stats.verified`, `declined` and `mismatched` itself.
  void (*verify)(Replacement& self, PPCFunc* original, PPCContext& ctx, uint8_t* base) = nullptr;
  Stats stats;
  // 0 unchecked, 1 guest code matches, -1 it does not (original only).
  std::atomic<int> code_state{0};
};

void Dispatch(Replacement& replacement, PPCFunc* original, PPCContext& ctx, uint8_t* base);

// Every replacement in the build, listed at static initialization so their
// guest code can be checked before the code bytes are retired (NN-6.7).
struct Declared {
  explicit Declared(Replacement& replacement);
};
// Checks the guest code of every declared replacement now, as its first call
// would; returns how many matched.
size_t CheckAllGuestCode(uint8_t* base);

// Writes one summary event per replacement that ran (route end, shutdown).
void RecordSummary();

// FNV-1a 64 over guest code bytes, as stored in Replacement.
uint64_t HashGuestCode(const uint8_t* code, uint32_t size);

}  // namespace lavender_shift::guest_native

#define LAVENDER_GUEST_NATIVE_JOIN2(a, b) a##b
#define LAVENDER_GUEST_NATIVE_JOIN(a, b) LAVENDER_GUEST_NATIVE_JOIN2(a, b)

// Replaces guest function sub_<hex> with `replacement`. Give the hex digits
// without 0x, e.g. 82448D90, and list the address in the analysis config's
// host_overridden_functions so direct calls reach it too.
#define LAVENDER_GUEST_NATIVE(hex, replacement)                                 \
  static ::lavender_shift::guest_native::Declared LAVENDER_GUEST_NATIVE_JOIN(   \
      lavender_guest_native_declared_, hex){replacement};                       \
  extern "C" REX_FUNC(LAVENDER_GUEST_NATIVE_JOIN(__imp__sub_, hex));            \
  extern "C" REX_FUNC(LAVENDER_GUEST_NATIVE_JOIN(sub_, hex)) {                  \
    ::lavender_shift::guest_native::Dispatch(                                   \
        replacement, &LAVENDER_GUEST_NATIVE_JOIN(__imp__sub_, hex), ctx, base); \
  }
