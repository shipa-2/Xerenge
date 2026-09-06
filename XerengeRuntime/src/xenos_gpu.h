#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

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
    void initializeRingBuffer(uint32_t guestAddress, uint32_t sizeLog2);
    void enableReadPointerWriteBack(uint32_t guestAddress, uint32_t blockSizeLog2);
    void processSubmittedBuffer(uint8_t* guestBase, uint32_t guestAddress, uint32_t dwordCount);
    void present(uint32_t width, uint32_t height);
    bool presentFromGuest(uint8_t* guestBase, uint32_t guestAddress, uint32_t width, uint32_t height);
    std::vector<uint8_t> framebufferCopy() const;
    uint64_t framebufferChecksum() const;

    uint64_t mmioWriteCount() const { return mmioWriteCount_; }
    uint64_t packetCount() const { return packetCount_; }
    uint64_t drawPacketCount() const { return drawPacketCount_; }
    uint64_t swapPacketCount() const { return swapPacketCount_; }
    uint64_t frameCount() const { return frameCount_; }
    uint32_t lastFrameWidth() const { return lastFrameWidth_; }
    uint32_t lastFrameHeight() const { return lastFrameHeight_; }
    uint64_t opcodeCount(uint32_t opcode) const { return opcodeCounts_[opcode & 0xFFu]; }
    uint32_t writePointer() const { return writePointer_; }
    bool ringConfigured() const { return ringBase_ != 0 && ringSizeDwords_ != 0; }

private:
    void processBuffer(uint8_t* guestBase, uint32_t guestAddress,
        uint32_t dwordCount, uint32_t recursionDepth);
    void processRing(uint8_t* guestBase);

    std::array<uint32_t, 0x2000> registers_{};
    // Xenos Type-0 packets address the 3D register file by dword index.
    // Keeping it separate from the MMIO register window lets draw handling
    // inspect the state accumulated through indirect command buffers.
    std::array<uint32_t, 0x8000> gpuRegisters_{};
    mutable std::recursive_mutex mutex_;
    std::vector<uint8_t> framebuffer_;
    uint32_t writePointer_ = 0;
    uint32_t readPointer_ = 0;
    uint32_t ringBase_ = 0;
    uint32_t ringSizeDwords_ = 0;
    uint32_t readPointerWriteback_ = 0;
    uint64_t mmioWriteCount_ = 0;
    uint64_t packetCount_ = 0;
    uint64_t type0Count_ = 0;
    uint64_t type3Count_ = 0;
    uint64_t drawPacketCount_ = 0;
    uint64_t swapPacketCount_ = 0;
    uint64_t frameCount_ = 0;
    uint32_t lastFrameWidth_ = 0;
    uint32_t lastFrameHeight_ = 0;
    std::array<uint64_t, 256> opcodeCounts_{};
};
