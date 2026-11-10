// Guest memory access for hooks that have no PPCContext: the Xbox 360 is
// big-endian, so values are swapped on load and store.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace lavender_shift::guest {

inline uint8_t* Pointer(uint32_t address) {
  auto* base = rex::system::kernel_state()->memory()->virtual_membase();
  return rex::memory::GuestPtr<uint8_t*>(base, address);
}

// Whether [address, address + size) is mapped and readable, by the guest
// heap's page table: for pointers read out of game structures.
inline bool Readable(uint32_t address, uint32_t size) {
  if (!address || !size || address + size - 1 < address) return false;
  auto* memory = rex::system::kernel_state()->memory();
  auto* heap = memory->LookupHeap(address);
  return heap &&
         heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

inline uint8_t LoadU8(uint32_t address) { return *Pointer(address); }
inline void StoreU8(uint32_t address, uint8_t value) { *Pointer(address) = value; }

inline uint32_t LoadU32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Pointer(address), sizeof(value));
  return std::byteswap(value);
}

inline void StoreU32(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(Pointer(address), &value, sizeof(value));
}

inline float LoadF32(uint32_t address) { return std::bit_cast<float>(LoadU32(address)); }
inline void StoreF32(uint32_t address, float value) {
  StoreU32(address, std::bit_cast<uint32_t>(value));
}

}  // namespace lavender_shift::guest
