#pragma once

// Where a guest address lives in host memory, for the hooks that read or write
// the title's memory from `base` themselves.
//
// Not simply base + address: on Windows (and on arm64 macOS) the physical
// mirror at 0xE0000000 and up is mapped 4 KB further on, because those hosts
// cannot map a view at a 4 KB offset. The recompiled code adds the same shift
// (REX_PHYS_HOST_OFFSET in the generated pch), so a hook that leaves it out
// reads and writes 4 KB away from what the title sees - harmless-looking on
// Linux, where the shift is 0, and a hang on Windows: the movie player's
// in-flight counter was misread there and drained without end.

#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace xerenge {

inline constexpr uint32_t PhysicalHostOffset(uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}

inline uint8_t* GuestPointer(uint8_t* base, uint32_t address) {
  return base + address + PhysicalHostOffset(address);
}

inline const uint8_t* GuestPointer(const uint8_t* base, uint32_t address) {
  return base + address + PhysicalHostOffset(address);
}

inline uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestPointer(base, address), sizeof(value));
  return __builtin_bswap32(value);
}

inline uint64_t LoadGuestU64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, GuestPointer(base, address), sizeof(value));
  return __builtin_bswap64(value);
}

inline void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(GuestPointer(base, address), &value, sizeof(value));
}

}  // namespace xerenge
