#pragma once
#include <cstdint>
#include <optional>

inline constexpr std::optional<uint32_t> guestHeapAllocationSize(
    uint32_t cursor, uint32_t limit, uint32_t size)
{
    const uint64_t aligned = (uint64_t(size) + 15u) & ~uint64_t(15u);
    if (cursor > limit || aligned > uint64_t(limit) - cursor)
        return std::nullopt;
    return static_cast<uint32_t>(aligned);
}
