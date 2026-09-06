#include "xenos_gpu.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
uint32_t loadGuestBE(const uint8_t* base, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
}

void storeGuestBE(uint8_t* base, uint32_t address, uint32_t value)
{
    const uint32_t encoded = __builtin_bswap32(value);
    std::memcpy(base + address, &encoded, sizeof(encoded));
}

}

void XenosGpu::present(uint32_t width, uint32_t height)
{
    std::lock_guard lock(mutex_);
    width = std::clamp(width, 1u, 4096u);
    height = std::clamp(height, 1u, 4096u);
    framebuffer_.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 3; i < framebuffer_.size(); i += 4)
        framebuffer_[i] = 255;
    lastFrameWidth_ = width;
    lastFrameHeight_ = height;
}

bool XenosGpu::presentFromGuest(uint8_t* guestBase, uint32_t guestAddress,
    uint32_t width, uint32_t height)
{
    std::lock_guard lock(mutex_);
    width = std::clamp(width, 1u, 4096u);
    height = std::clamp(height, 1u, 4096u);
    const size_t byteCount = static_cast<size_t>(width) * height * 4;
    if (guestAddress == 0 || guestAddress > 0xFFFFFFFFu - byteCount)
        return false;

    // The first bring-up path uses the guest surface as a linear X8R8G8B8
    // readback.  Xenos tiling/swizzle is handled separately once command
    // packets identify the render-target format; keeping the copy here makes
    // a title-provided frontbuffer observable without fabricating pixels.
    framebuffer_.resize(byteCount);
    std::memcpy(framebuffer_.data(), guestBase + guestAddress, byteCount);
    for (size_t i = 0; i < framebuffer_.size(); i += 4)
        framebuffer_[i + 3] = 255;
    lastFrameWidth_ = width;
    lastFrameHeight_ = height;
    return true;
}

std::vector<uint8_t> XenosGpu::framebufferCopy() const
{
    std::lock_guard lock(mutex_);
    return framebuffer_;
}

uint64_t XenosGpu::framebufferChecksum() const
{
    std::lock_guard lock(mutex_);
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : framebuffer_)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void XenosGpu::initializeRingBuffer(uint32_t guestAddress, uint32_t sizeLog2)
{
    std::lock_guard lock(mutex_);
    ringBase_ = guestAddress;
    ringSizeDwords_ = sizeLog2 < 31 ? (1u << sizeLog2) : 0;
    readPointer_ = 0;
    writePointer_ = 0;
}

void XenosGpu::enableReadPointerWriteBack(uint32_t guestAddress, uint32_t)
{
    std::lock_guard lock(mutex_);
    readPointerWriteback_ = guestAddress;
}

void XenosGpu::processSubmittedBuffer(uint8_t* guestBase, uint32_t guestAddress, uint32_t dwordCount)
{
    std::lock_guard lock(mutex_);
    uint32_t offset = 0;
    while (offset < dwordCount)
    {
        const uint32_t packet = loadGuestBE(guestBase, guestAddress + offset * 4);
        if (packet == 0)
        {
            ++offset;
            continue;
        }
        const uint32_t type = packet >> 30;
        uint32_t length = 1;
        if (type == 0)
        {
            ++type0Count_;
            length = ((packet >> 16) & 0x3FFFu) + 1;
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            // Xenos PM4 draw packets.  The low seven bits are used by the
            // hardware opcode field; accepting both forms keeps this parser
            // useful for command streams produced by different compilers.
            if (opcode == 0x22u || opcode == 0x23u || opcode == 0x2Du ||
                opcode == 0x2Eu || opcode == 0x36u)
                ++drawPacketCount_;
            if (opcode == 0x64u)
            {
                ++swapPacketCount_;
                if (length >= 5 && offset + 4 < dwordCount)
                {
                    const uint32_t width = loadGuestBE(guestBase, guestAddress + (offset + 3) * 4);
                    const uint32_t height = loadGuestBE(guestBase, guestAddress + (offset + 4) * 4);
                    lastFrameWidth_ = width;
                    lastFrameHeight_ = height;
                    ++frameCount_;
                }
            }
        }
        if (length == 0 || length > dwordCount - offset)
            break;
        ++packetCount_;
        offset += length;
    }
}

void XenosGpu::write(uint8_t* guestBase, uint32_t address, uint64_t value, uint32_t width)
{
    std::lock_guard lock(mutex_);
    ++mmioWriteCount_;
    if (address < kMmioBase || address >= kMmioBase + kMmioSize || width != 4)
        return;

    const uint32_t index = (address - kMmioBase) / 4;
    if (index >= registers_.size())
        return;

    const uint32_t registerValue = static_cast<uint32_t>(value);
    registers_[index] = registerValue;
    if (index == kCpRbBase)
    {
        ringBase_ = registerValue;
    }
    else if (index == kCpRbCntl)
    {
        const uint32_t sizeLog2 = registerValue & 0x3Fu;
        ringSizeDwords_ = sizeLog2 < 31 ? (1u << sizeLog2) : 0;
    }
    else if (index == kCpRbRptrAddr)
    {
        readPointerWriteback_ = registerValue;
    }
    else if (index == kCpRbWptr)
    {
        writePointer_ = registerValue;
        if (std::getenv("XERENGE_PPC_MMIO_TRACE") != nullptr && writePointer_ <= 0x80)
        {
            std::cerr << "Xenos ring base=0x" << std::hex << ringBase_
                      << " wptr=0x" << writePointer_ << ":";
            for (uint32_t i = 0; i < std::min<uint32_t>(writePointer_, 32); ++i)
                std::cerr << " " << loadGuestBE(guestBase, ringBase_ + i * 4);
            std::cerr << std::dec << '\n';
        }
        processRing(guestBase);
    }
}

void XenosGpu::processRing(uint8_t* guestBase)
{
    if (ringBase_ == 0 || ringSizeDwords_ == 0)
    {
        if (std::getenv("XERENGE_PPC_MMIO_TRACE") != nullptr && mmioWriteCount_ <= 4)
            std::cerr << "Xenos CP_RB_WPTR=" << writePointer_
                      << " without configured ring buffer\n";
        return;
    }

    const uint32_t target = std::min(writePointer_, ringSizeDwords_);
    while (readPointer_ < target)
    {
        const uint32_t packet = loadGuestBE(guestBase, ringBase_ + readPointer_ * 4);
        const uint32_t type = packet >> 30;
        uint32_t length = 1;
        if (type == 0)
        {
            ++type0Count_;
            length = ((packet >> 16) & 0x3FFFu) + 1;
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            if (opcode == 0x22u || opcode == 0x23u || opcode == 0x2Du ||
                opcode == 0x2Eu || opcode == 0x36u)
                ++drawPacketCount_;
            if (opcode == 0x64u)
            {
                ++swapPacketCount_;
                if (length >= 5 && readPointer_ + 4 < target)
                {
                    lastFrameWidth_ = loadGuestBE(guestBase,
                        ringBase_ + (readPointer_ + 3) * 4);
                    lastFrameHeight_ = loadGuestBE(guestBase,
                        ringBase_ + (readPointer_ + 4) * 4);
                    ++frameCount_;
                }
            }
        }

        if (length == 0 || length > target - readPointer_)
            break;
        ++packetCount_;
        readPointer_ += length;
    }

    // The read pointer writeback is the first synchronization primitive the
    // guest observes.  Use the same guest endian representation as PPC stores.
    if (readPointerWriteback_ != 0)
        storeGuestBE(guestBase, readPointerWriteback_, readPointer_);
}
