#include "xenos_gpu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "shader_cache_runtime.h"

namespace
{
constexpr uint32_t kEdramSize = 10u * 1024u * 1024u;

uint32_t gpuPhysicalToGuest(uint32_t physicalAddress)
{
    return 0x60000000u | (physicalAddress & 0x1FFFFFFFu);
}

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
    if (std::getenv("XERENGE_FRAMEBUFFER_TRACE") != nullptr)
    {
        size_t nonzeroRgbPixels = 0;
        for (size_t i = 0; i + 3 < framebuffer_.size(); i += 4)
            nonzeroRgbPixels += (framebuffer_[i] | framebuffer_[i + 1] |
                framebuffer_[i + 2]) != 0;
        std::cerr << "Xenos frontbuffer guest=0x" << std::hex << guestAddress
                  << " bytes=" << std::dec << byteCount
                  << " nonzeroRgbPixels=" << nonzeroRgbPixels << '\n';
    }
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
    {
        gpuRegisters_[index] = value;
        if (index == 0x2104u && std::getenv("XERENGE_XENOS_MASK_TRACE") != nullptr)
            std::cerr << "Xenos RB_COLOR_MASK=0x" << std::hex << value << std::dec << '\n';
    }
}

void XenosGpu::rememberVertexFetchStrides(const uint32_t* code, uint32_t dwordCount)
{
    // Immediate Xenos vertex shaders are triples of dwords.  The stride is
    // carried by the third word of a full vfetch instruction, while the
    // fetch constant index is carried by the first word.  Recording this
    // small piece of metadata lets the bootstrap rasterizer use the same
    // layout as the shader without hard-coding a program id.
    for (uint32_t offset = 0; offset + 2 < dwordCount; offset += 3)
    {
        const uint32_t word0 = code[offset];
        if ((word0 & 0x1Fu) != 0u) // FetchOpcode::VertexFetch.
            continue;
        const uint32_t constantIndex =
            (((word0 >> 20) & 0x1Fu) * 3u) + ((word0 >> 25) & 0x3u);
        const uint32_t stride = code[offset + 2] & 0xFFu;
        if (constantIndex < vertexFetchStrideWords_.size() && stride != 0u)
            vertexFetchStrideWords_[constantIndex] = stride;
    }
}

void XenosGpu::rasterizeDraw(uint8_t* guestBase, uint32_t initiator)
{
    // Burnout uses auto-indexed point draws during bootstrap and three-vertex
    // primitive-8 draws for the first render-target geometry. Copy packets
    // remain separate from this raster path.
    const uint32_t primitive = initiator & 0x3Fu;
    const uint32_t source = (initiator >> 6) & 0x3u;
    const uint32_t count = initiator >> 16;
    if ((primitive != 1u && primitive != 8u) || source != 2u || count == 0 ||
        gpuRegisters_[0x2318] != 0 || gpuRegisters_[0x2104] == 0)
        return;

    const uint32_t fetch0 = gpuRegisters_[0x4800];
    const uint32_t fetch1 = gpuRegisters_[0x4801];
    if ((fetch0 & 0x3u) != 3u)
        return;

    const uint32_t physicalAddress = (fetch0 >> 2) << 2;
    const uint32_t vertexAddress = gpuPhysicalToGuest(physicalAddress);
    // The fetch constant's second word is the buffer size, not the vertex
    // stride.  Xenos encodes the stride in each vfetch instruction; the
    // bootstrap VS loaded by Burnout uses a seven-dword vertex and its
    // constant therefore remains zero in the register file.  The metadata is
    // populated when an immediate vertex shader is received below.
    uint32_t strideWords = vertexFetchStrideWords_[0];
    if (strideWords == 0u)
        strideWords = (fetch1 >> 2) & 0xFFFFFFu;
    const uint32_t minimumStride = primitive == 8u ? 2u : 3u;
    if (strideWords < minimumStride || strideWords > 0x1000)
        return;

    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    if (edram_.size() != size_t(width) * height * 4)
        edram_.assign(size_t(width) * height * 4, 0);

    const uint32_t firstIndex = gpuRegisters_[0x2102] & 0x00FFFFFFu;
    const uint32_t drawVertices = primitive == 8u ? std::min(count, 3u) : count;
    std::array<std::array<float, 3>, 3> triangle{};
    std::array<uint8_t, 4> drawColor{255, 255, 255, 255};
    bool havePixelConstant = false;
    for (uint32_t component = 0; component < 4; ++component)
    {
        const uint32_t raw = gpuRegisters_[0x4940u + component];
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        if (std::isfinite(value) && std::abs(value) > 0.0001f)
            havePixelConstant = true;
        if (std::isfinite(value))
            drawColor[component] = static_cast<uint8_t>(
                std::clamp(value, 0.0f, 1.0f) * 255.0f);
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t address = vertexAddress +
            (firstIndex + i) * strideWords * sizeof(uint32_t);
        uint32_t bits[3]{};
        for (uint32_t component = 0; component < 3; ++component)
            bits[component] = loadGuestBE(guestBase, address + component * 4);

        float position[3];
        std::memcpy(&position[0], &bits[0], sizeof(position));
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
            !std::isfinite(position[2]) || position[2] < -1.0f || position[2] > 1.0f)
            continue;

        const float xNdc = position[0];
        const float yNdc = position[1];
        if (xNdc < -1.0f || xNdc > 1.0f || yNdc < -1.0f || yNdc > 1.0f)
            continue;

        if (primitive == 8u && i < triangle.size())
            triangle[i] = {xNdc, yNdc, position[2]};
        if (primitive == 8u)
            continue;

        const uint32_t x = std::min(width - 1,
            static_cast<uint32_t>((xNdc * 0.5f + 0.5f) * width));
        const uint32_t y = std::min(height - 1,
            static_cast<uint32_t>((1.0f - (yNdc * 0.5f + 0.5f)) * height));
        const size_t pixel = (size_t(y) * width + x) * 4;
        // The bootstrap vertex program's second fetch is a float4
        // interpolator at words 3..6. The matching cached pixel program
        // forwards that interpolator to color 0, so preserve those actual
        // guest values instead of using a diagnostic white fragment.
        uint8_t color[4]{};
        for (uint32_t component = 0; component < 4; ++component)
        {
            const uint32_t raw = loadGuestBE(guestBase, address + (3 + component) * 4);
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            if (std::isfinite(value))
                color[component] = static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        }
        std::memcpy(edram_.data() + pixel, color, sizeof(color));
    }

    if (primitive == 8u && drawVertices == 3u)
    {
        const float minX = std::min({triangle[0][0], triangle[1][0], triangle[2][0]});
        const float maxX = std::max({triangle[0][0], triangle[1][0], triangle[2][0]});
        const float minY = std::min({triangle[0][1], triangle[1][1], triangle[2][1]});
        const float maxY = std::max({triangle[0][1], triangle[1][1], triangle[2][1]});
        const int left = std::max(0, static_cast<int>((minX * 0.5f + 0.5f) * width));
        const int right = std::min(static_cast<int>(width) - 1,
            static_cast<int>((maxX * 0.5f + 0.5f) * width));
        const int top = std::max(0, static_cast<int>((1.0f - (maxY * 0.5f + 0.5f)) * height));
        const int bottom = std::min(static_cast<int>(height) - 1,
            static_cast<int>((1.0f - (minY * 0.5f + 0.5f)) * height));
        const float ax = triangle[0][0], ay = triangle[0][1];
        const float bx = triangle[1][0], by = triangle[1][1];
        const float cx = triangle[2][0], cy = triangle[2][1];
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (area != 0.0f)
        {
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x)
                {
                    const float px = (static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f;
                    const float py = 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f;
                    const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                        std::memcpy(edram_.data() + (size_t(y) * width + x) * 4,
                            drawColor.data(), drawColor.size());
                }
        }
        if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
            std::cerr << "Xenos triangle rasterized v0=" << triangle[0][0] << ','
                      << triangle[0][1] << " v1=" << triangle[1][0] << ','
                      << triangle[1][1] << " v2=" << triangle[2][0] << ','
                      << triangle[2][1] << " pixelConstant="
                      << (havePixelConstant ? "yes" : "fallback") << '\n';
    }
}

void XenosGpu::resolveToGuest(uint8_t* guestBase)
{
    const uint32_t physicalDestination = gpuRegisters_[0x2319];
    if (physicalDestination == 0)
        return;

    if (edram_.empty())
        edram_.resize(kEdramSize, 0);

    // RB_COPY_DEST_BASE is a 29-bit physical address. The runtime maps the
    // title's physical allocations through the 0x60000000 guest alias.
    const uint32_t destination =
        0x60000000u | (physicalDestination & 0x1FFFFFFFu);
    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    constexpr size_t byteCount = size_t(width) * height * 4;
    if (destination > 0xFFFFFFFFu - byteCount)
        return;

    // The linear path is deliberately limited to the portion of EDRAM that
    // exists in this bring-up. Once the software rasterizer writes tiled
    // samples, this is replaced by the Xenos tile resolve routine.
    const size_t copyCount = std::min(byteCount, edram_.size());
    std::memcpy(guestBase + destination, edram_.data(), copyCount);
    ++resolveCount_;
    if (std::getenv("XERENGE_XENOS_RESOLVE_TRACE") != nullptr)
    {
        size_t nonzeroPixels = 0;
        for (size_t i = 0; i + 3 < copyCount; i += 4)
            nonzeroPixels += (edram_[i] | edram_[i + 1] | edram_[i + 2]) != 0;
        std::cerr << "Xenos resolve destination=0x" << std::hex << destination
                  << " bytes=" << std::dec << copyCount
                  << " nonzeroRgbPixels=" << nonzeroPixels << '\n';
    }
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
            if (opcode == 0x2Fu && length >= 4 && offset + 3 < dwordCount)
            {
                // PM4_LOAD_ALU_CONSTANT: physical source address, typed
                // destination register, and dword count.
                const uint32_t physicalAddress = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4) & 0x3FFFFFFFu;
                const uint32_t offsetType = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4);
                const uint32_t sizeDwords = loadGuestBE(
                    guestBase, guestAddress + (offset + 3) * 4) & 0xFFFu;
                uint32_t index = offsetType & 0x7FFu;
                switch ((offsetType >> 16) & 0xFFu)
                {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = 0xFFFFFFFFu; break;
                }
                const uint32_t source = gpuPhysicalToGuest(physicalAddress);
                if (index != 0xFFFFFFFFu && sizeDwords <= 0x8000u - index)
                    for (uint32_t i = 0; i < sizeDwords; ++i)
                        writeGpuRegister(index + i,
                            loadGuestBE(guestBase, source + i * 4));
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
            if (opcode == 0x2Bu && length >= 3 && offset + 2 < dwordCount)
            {
                const uint32_t shaderType = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                const uint32_t codeDwords = loadGuestBE(
                    guestBase, guestAddress + (offset + 2) * 4) & 0xFFFFu;
                if (codeDwords != 0 && codeDwords <= length - 3)
                {
                    if (std::getenv("XERENGE_XENOS_SHADER_DUMP") != nullptr)
                    {
                        std::cerr << "Xenos shader code stage=" << shaderType
                                  << " dwords=" << codeDwords << ':';
                        for (uint32_t i = 0; i < codeDwords; ++i)
                            std::cerr << " " << std::hex << loadGuestBE(
                                guestBase, guestAddress + (offset + 3 + i) * 4);
                        std::cerr << std::dec << '\n';
                    }
                    if (shaderType == 0u)
                    {
                        std::array<uint32_t, 1024> code{};
                        const uint32_t copied = std::min<uint32_t>(codeDwords, code.size());
                        for (uint32_t i = 0; i < copied; ++i)
                            code[i] = loadGuestBE(
                                guestBase, guestAddress + (offset + 3 + i) * 4);
                        rememberVertexFetchStrides(code.data(), copied);
                    }
                    const size_t byteSize = size_t(codeDwords) * sizeof(uint32_t);
                    const uint64_t hash = XXH3_64bits(
                        guestBase + guestAddress + (offset + 3) * 4, byteSize);
                    const auto cache = xerengeShaderCache();
                    const auto* match = cache.findMicrocode(hash);
                    if (std::getenv("XERENGE_XENOS_SHADER_TRACE") != nullptr)
                        std::cerr << "Xenos shader cache "
                                  << (match != nullptr ? "hit" : "miss")
                                  << " hash=0x" << std::hex << hash
                                  << " stage=" << shaderType
                                  << " bytes=" << std::dec << byteSize;
                        if (match != nullptr)
                            std::cerr << " compiled=0x" << std::hex << match->shaderHash;
                        std::cerr << std::dec << '\n';
                }
            }
            if (opcode == 0x46u && length >= 2 && offset + 1 < dwordCount)
            {
                const uint32_t event = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                if (event == 6u)
                    resolveToGuest(guestBase);
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
            if (opcode == 0x36u && length >= 2 && offset + 1 < dwordCount)
            {
                // DRAW_INDX_2 carries VGT_DRAW_INITIATOR as its only payload.
                // Keep it in the register file so later rasterization sees the
                // same state as a SET_CONSTANT/Type-0 packet would provide.
                gpuRegisters_[0x21FC] = loadGuestBE(
                    guestBase, guestAddress + (offset + 1) * 4);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
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
                    const uint32_t initiator = gpuRegisters_[0x21FC];
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " mode=0x" << gpuRegisters_[0x2208]
                              << " depth=0x" << gpuRegisters_[0x2200]
                              << " initiator=0x" << initiator
                              << " prim=" << (initiator & 0x3Fu)
                              << " source=" << ((initiator >> 6) & 0x3u)
                              << " indices=" << (initiator >> 16)
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << " copyCtl=0x" << gpuRegisters_[0x2318]
                              << " copyPitch=0x" << gpuRegisters_[0x231A]
                              << " copyInfo=0x" << gpuRegisters_[0x231B]
                              << " clear=0x" << gpuRegisters_[0x231E]
                              << std::dec << '\n';
                    if (std::getenv("XERENGE_XENOS_FETCH_TRACE") != nullptr)
                    {
                        std::cerr << "Xenos fetch0=" << std::hex;
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4800 + i];
                        std::cerr << " fetch1=";
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4806 + i];
                        std::cerr << std::dec << '\n';
                        if (std::getenv("XERENGE_XENOS_CONSTANT_TRACE") != nullptr)
                        {
                            std::cerr << "Xenos vs constants:" << std::hex;
                            for (uint32_t i = 0; i < 24; ++i)
                                std::cerr << " " << gpuRegisters_[0x4000 + i];
                            std::cerr << std::dec << '\n';
                        }
                        const uint32_t fetch0 = gpuRegisters_[0x4800];
                        const uint32_t fetch1 = gpuRegisters_[0x4801];
                        if ((fetch0 & 0x3u) == 3u)
                        {
                            const uint32_t physicalAddress = (fetch0 >> 2) << 2;
                            const uint32_t address = gpuPhysicalToGuest(physicalAddress);
                            const uint32_t words = (fetch1 >> 2) & 0xFFFFFFu;
                            std::cerr << "Xenos vertex-data physical=0x" << std::hex
                                      << physicalAddress << " guest=0x" << address
                                      << " words=" << std::dec << words << ':';
                            for (uint32_t vertex = 0; vertex < 3; ++vertex)
                            {
                                std::cerr << " [";
                                for (uint32_t word = 0; word < words; ++word)
                                    std::cerr << (word ? " " : "") << std::hex
                                              << loadGuestBE(guestBase,
                                                  address + (vertex * words + word) * 4);
                                std::cerr << "]";
                            }
                            std::cerr << std::dec << '\n';
                        }
                    }
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
            if (opcode == 0x2Fu && length >= 4 && readPointer_ + 3 < target)
            {
                const uint32_t physicalAddress = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 1) * 4) & 0x3FFFFFFFu;
                const uint32_t offsetType = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 2) * 4);
                const uint32_t sizeDwords = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 3) * 4) & 0xFFFu;
                uint32_t index = offsetType & 0x7FFu;
                switch ((offsetType >> 16) & 0xFFu)
                {
                case 0: index += 0x4000; break;
                case 1: index += 0x4800; break;
                case 2: index += 0x4900; break;
                case 3: index += 0x4908; break;
                case 4: index += 0x2000; break;
                default: index = 0xFFFFFFFFu; break;
                }
                const uint32_t source = gpuPhysicalToGuest(physicalAddress);
                if (index != 0xFFFFFFFFu && sizeDwords <= 0x8000u - index)
                    for (uint32_t i = 0; i < sizeDwords; ++i)
                        writeGpuRegister(index + i,
                            loadGuestBE(guestBase, source + i * 4));
            }
            if (opcode == 0x46u && length >= 2 && readPointer_ + 1 < target)
            {
                const uint32_t event = loadGuestBE(guestBase,
                    ringBase_ + (readPointer_ + 1) * 4);
                if (event == 6u)
                    resolveToGuest(guestBase);
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
            if (opcode == 0x36u && length >= 2 && readPointer_ + 1 < target)
            {
                gpuRegisters_[0x21FC] = loadGuestBE(
                    guestBase, ringBase_ + (readPointer_ + 1) * 4);
                rasterizeDraw(guestBase, gpuRegisters_[0x21FC]);
            }
            if (opcode == 0x22u || opcode == 0x23u || opcode == 0x2Du ||
                opcode == 0x2Eu || opcode == 0x36u)
            {
                ++drawPacketCount_;
                if (std::getenv("XERENGE_XENOS_DRAW_TRACE") != nullptr)
                {
                    const uint32_t initiator = gpuRegisters_[0x21FC];
                    std::cerr << "Xenos draw state surface=0x" << std::hex
                              << gpuRegisters_[0x2000]
                              << " color=0x" << gpuRegisters_[0x2001]
                              << " mask=0x" << gpuRegisters_[0x2104]
                              << " mode=0x" << gpuRegisters_[0x2208]
                              << " depth=0x" << gpuRegisters_[0x2200]
                              << " initiator=0x" << initiator
                              << " prim=" << (initiator & 0x3Fu)
                              << " source=" << ((initiator >> 6) & 0x3u)
                              << " indices=" << (initiator >> 16)
                              << " program=0x" << gpuRegisters_[0x2180]
                              << " vsConst=0x" << gpuRegisters_[0x2307]
                              << " psConst=0x" << gpuRegisters_[0x2308]
                              << " copyBase=0x" << gpuRegisters_[0x2319]
                              << " copyCtl=0x" << gpuRegisters_[0x2318]
                              << " copyPitch=0x" << gpuRegisters_[0x231A]
                              << " copyInfo=0x" << gpuRegisters_[0x231B]
                              << " clear=0x" << gpuRegisters_[0x231E]
                              << std::dec << '\n';
                    if (std::getenv("XERENGE_XENOS_FETCH_TRACE") != nullptr)
                    {
                        std::cerr << "Xenos fetch0=" << std::hex;
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4800 + i];
                        std::cerr << " fetch1=";
                        for (uint32_t i = 0; i < 6; ++i)
                            std::cerr << " " << gpuRegisters_[0x4806 + i];
                        std::cerr << std::dec << '\n';
                        if (std::getenv("XERENGE_XENOS_CONSTANT_TRACE") != nullptr)
                        {
                            std::cerr << "Xenos vs constants:" << std::hex;
                            for (uint32_t i = 0; i < 24; ++i)
                                std::cerr << " " << gpuRegisters_[0x4000 + i];
                            std::cerr << std::dec << '\n';
                        }
                        const uint32_t fetch0 = gpuRegisters_[0x4800];
                        const uint32_t fetch1 = gpuRegisters_[0x4801];
                        if ((fetch0 & 0x3u) == 3u)
                        {
                            const uint32_t physicalAddress = (fetch0 >> 2) << 2;
                            const uint32_t address = gpuPhysicalToGuest(physicalAddress);
                            const uint32_t words = (fetch1 >> 2) & 0xFFFFFFu;
                            std::cerr << "Xenos vertex-data physical=0x" << std::hex
                                      << physicalAddress << " guest=0x" << address
                                      << " words=" << std::dec << words << ':';
                            for (uint32_t vertex = 0; vertex < 3; ++vertex)
                            {
                                std::cerr << " [";
                                for (uint32_t word = 0; word < words; ++word)
                                    std::cerr << (word ? " " : "") << std::hex
                                              << loadGuestBE(guestBase,
                                                  address + (vertex * words + word) * 4);
                                std::cerr << "]";
                            }
                            std::cerr << std::dec << '\n';
                        }
                    }
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
