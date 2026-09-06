#pragma once

#include <array>
#include <cstdint>

class XenosGpu
{
public:
    static constexpr uint32_t kMmioBase = 0x7FC80000u;
    static constexpr uint32_t kMmioSize = 0x10000u;
    static constexpr uint32_t kCpRbBase = 0x1C0u;
    static constexpr uint32_t kCpRbCntl = 0x1C1u;
    static constexpr uint32_t kCpRbRptrAddr = 0x1C3u;
    static constexpr uint32_t kCpRbWptr = 0x1C5u;

    void write(uint8_t* guestBase, uint32_t address, uint64_t value, uint32_t width);

    uint64_t mmioWriteCount() const { return mmioWriteCount_; }
    uint64_t packetCount() const { return packetCount_; }
    uint32_t writePointer() const { return writePointer_; }

private:
    void processRing(uint8_t* guestBase);

    std::array<uint32_t, 0x2000> registers_{};
    uint32_t writePointer_ = 0;
    uint32_t readPointer_ = 0;
    uint32_t ringBase_ = 0;
    uint32_t ringSizeDwords_ = 0;
    uint32_t readPointerWriteback_ = 0;
    uint64_t mmioWriteCount_ = 0;
    uint64_t packetCount_ = 0;
    uint64_t type0Count_ = 0;
    uint64_t type3Count_ = 0;
};
