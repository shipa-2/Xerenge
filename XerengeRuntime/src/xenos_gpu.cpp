#include "xenos_gpu.h"

#include <algorithm>
#include <atomic>
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

void XenosGpu::writeGpuRegister(uint32_t index, uint32_t value)
{
    if (index < gpuRegisters_.size())
        gpuRegisters_[index] = value;
}

void XenosGpu::processSubmittedBuffer(uint8_t* guestBase, uint32_t guestAddress, uint32_t dwordCount)
{
    std::lock_guard lock(mutex_);
    processBuffer(guestBase, guestAddress, dwordCount, 0);
}

void XenosGpu::processBuffer(uint8_t* guestBase, uint32_t guestAddress,
    uint32_t dwordCount, uint32_t recursionDepth)
{
    if (recursionDepth > 8 || dwordCount > (1u << 22))
        return;

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
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t baseRegister = packet & 0x7FFFu;
            const bool writeOne = (packet & 0x8000u) != 0;
            for (uint32_t i = 0; i + 1 < length; ++i)
            {
                const uint32_t index = writeOne ? baseRegister : baseRegister + i;
                if (index < gpuRegisters_.size())
                    gpuRegisters_[index] = loadGuestBE(
                        guestBase, guestAddress + (offset + 1 + i) * 4);
            }
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            if (opcode == 0x2Du || opcode == 0x55u || opcode == 0x56u)
            {
                const uint32_t payloadCount = length - 1;
                if (payloadCount != 0 && offset + payloadCount < dwordCount)
                {
                    const uint32_t offsetType = loadGuestBE(
                        guestBase, guestAddress + (offset + 1) * 4);
                    uint32_t index = offsetType & (opcode == 0x2Du ? 0x7FFu : 0xFFFFu);
                    if (opcode == 0x2Du)
                    {
                        switch ((offsetType >> 16) & 0xFFu)
                        {
                        case 0: index += 0x4000; break;
                        case 1: index += 0x4800; break;
                        case 2: index += 0x4900; break;
                        case 3: index += 0x4908; break;
                        case 4: index += 0x2000; break;
                        default: index = 0xFFFFFFFFu; break;
                        }
                    }
                    if (index != 0xFFFFFFFFu)
                        for (uint32_t i = 1; i < payloadCount; ++i)
                            writeGpuRegister(index + i - 1,
                                loadGuestBE(guestBase, guestAddress + (offset + 1 + i) * 4));
                }
            }
            if (opcode == 0x2Bu && length >= 3 && offset + 2 < dwordCount &&
                std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
            {
                const uint32_t shaderType = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t startSize = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                std::cerr << "Xenos immediate shader type=" << shaderType
                          << " dwords=" << (startSize & 0xFFFFu) << '\n';
            }
            if (std::getenv("XERENGE_XENOS_PACKET_TRACE") != nullptr)
            {
                static std::atomic<uint32_t> traceCount = 0;
                if (traceCount.fetch_add(1, std::memory_order_relaxed) < 256)
                {
                    std::cerr << "Xenos PM4 depth=" << recursionDepth
                              << " address=0x" << std::hex << guestAddress + offset * 4
                              << " opcode=0x" << opcode << std::dec
                              << " dwords=" << length;
                    for (uint32_t i = 1; i < std::min<uint32_t>(length, 9); ++i)
                        std::cerr << " " << std::hex
                                  << loadGuestBE(guestBase, guestAddress + (offset + i) * 4);
                    std::cerr << std::dec << '\n';
                }
            }
            if (opcode == 0x3Fu && length >= 3 && offset + 2 < dwordCount)
            {
                // CP_INDIRECT_BUFFER stores a 29-bit physical address.  The
                // title's physical allocations live in the 0x60000000 guest
                // alias, while PM4 strips those high virtual-address bits.
                const uint32_t physicalAddress =
                    loadGuestBE(guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t indirectCount =
                    loadGuestBE(guestBase, guestAddress + (offset + 2) * 4) & 0xFFFFFu;
                const uint32_t indirectAddress =
                    0x60000000u | (physicalAddress & 0x1FFFFFFFu);
                processBuffer(guestBase, indirectAddress, indirectCount, recursionDepth + 1);
            }
            // Xenos PM4 draw packets.  The low seven bits are used by the
            // hardware opcode field; accepting both forms keeps this parser
            // useful for command streams produced by different compilers.
            if (opcode == 0x22u || opcode == 0x23u || opcode == 0x2Du ||
                opcode == 0x2Eu || opcode == 0x36u)
            {
                ++drawPacketCount_;
                if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                {
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " initiator=0x" << gpuRegisters_[0x21FC]
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << std::dec << '\n';
                }
            }
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
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t baseRegister = packet & 0x7FFFu;
            const bool writeOne = (packet & 0x8000u) != 0;
            for (uint32_t i = 0; i + 1 < length; ++i)
            {
                const uint32_t index = writeOne ? baseRegister : baseRegister + i;
                if (index < gpuRegisters_.size())
                    gpuRegisters_[index] = loadGuestBE(
                        guestBase, ringBase_ + (readPointer_ + 1 + i) * 4);
            }
        }
        else if (type == 3)
        {
            ++type3Count_;
            length = ((packet >> 16) & 0x3FFFu) + 2;
            const uint32_t opcode = (packet >> 8) & 0xFFu;
            ++opcodeCounts_[opcode];
            if (opcode == 0x2Du || opcode == 0x55u || opcode == 0x56u)
            {
                const uint32_t payloadCount = length - 1;
                if (payloadCount != 0 && readPointer_ + payloadCount < target)
                {
                    const uint32_t offsetType = loadGuestBE(guestBase,
                        ringBase_ + (readPointer_ + 1) * 4);
                    uint32_t index = offsetType & (opcode == 0x2Du ? 0x7FFu : 0xFFFFu);
                    if (opcode == 0x2Du)
                    {
                        switch ((offsetType >> 16) & 0xFFu)
                        {
                        case 0: index += 0x4000; break;
                        case 1: index += 0x4800; break;
                        case 2: index += 0x4900; break;
                        case 3: index += 0x4908; break;
                        case 4: index += 0x2000; break;
                        default: index = 0xFFFFFFFFu; break;
                        }
                    }
                    if (index != 0xFFFFFFFFu)
                        for (uint32_t i = 1; i < payloadCount; ++i)
                            writeGpuRegister(index + i - 1,
                                loadGuestBE(guestBase,
                                    ringBase_ + (readPointer_ + 1 + i) * 4));
                }
            }
            if (opcode == 0x3Fu && length >= 3 && readPointer_ + 2 < target)
            {
                const uint32_t physicalAddress = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 1) * 4);
                const uint32_t indirectCount = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 2) * 4) & 0xFFFFFu;
                const uint32_t indirectAddress =
                    0x60000000u | (physicalAddress & 0x1FFFFFFFu);
                processBuffer(guestBase, indirectAddress, indirectCount, 1);
            }
            if (opcode == 0x22u || opcode == 0x23u || opcode == 0x2Du ||
                opcode == 0x2Eu || opcode == 0x36u)
            {
                ++drawPacketCount_;
                if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                {
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " initiator=0x" << gpuRegisters_[0x21FC]
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << std::dec << '\n';
                }
            }
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
