#include "shader_cache_runtime.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <zstd.h>
#include <smolv.h>

#ifndef XERENGE_HAS_SHADER_CACHE
ShaderCacheEntry g_shaderCacheEntries[1]{};
const size_t g_shaderCacheEntryCount = 0;
const uint8_t g_compressedSpirvCache[1]{};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
ShaderMicrocodeEntry g_shaderMicrocodeEntries[1]{};
const size_t g_shaderMicrocodeEntryCount = 0;
#endif

XerengeShaderCacheView xerengeShaderCache()
{
    return {
        g_shaderCacheEntries,
        g_shaderCacheEntryCount,
        g_compressedSpirvCache,
        g_spirvCacheCompressedSize,
        g_spirvCacheDecompressedSize,
    };
}

const ShaderCacheEntry* XerengeShaderCacheView::find(uint64_t hash) const
{
    if (!available())
        return nullptr;
    const auto* end = entries + entryCount;
    const auto* it = std::lower_bound(entries, end, hash,
        [](const ShaderCacheEntry& entry, uint64_t value) { return entry.hash < value; });
    return it != end && it->hash == hash ? it : nullptr;
}

const ShaderMicrocodeEntry* XerengeShaderCacheView::findMicrocode(uint64_t hash) const
{
    if (g_shaderMicrocodeEntryCount == 0)
        return nullptr;
    const auto* begin = g_shaderMicrocodeEntries;
    const auto* end = begin + g_shaderMicrocodeEntryCount;
    const auto* it = std::lower_bound(begin, end, hash,
        [](const ShaderMicrocodeEntry& entry, uint64_t value) {
            return entry.microcodeHash < value;
        });
    return it != end && it->microcodeHash == hash ? it : nullptr;
}

const uint32_t* XerengeShaderCacheView::spirv(
    const ShaderCacheEntry& entry, size_t& wordCount) const
{
    wordCount = 0;
    if (compressedSpirv == nullptr || compressedSpirvSize == 0 ||
        decompressedSpirvSize == 0 || entry.spirvSize == 0 ||
        entry.spirvOffset > decompressedSpirvSize ||
        entry.spirvSize > decompressedSpirvSize - entry.spirvOffset)
        return nullptr;

    static std::once_flag once;
    static std::vector<uint8_t> cache;
    static bool valid = false;
    std::call_once(once, [&]
    {
        cache.resize(decompressedSpirvSize);
        const size_t result = ZSTD_decompress(cache.data(), decompressedSpirvSize,
            compressedSpirv, compressedSpirvSize);
        valid = !ZSTD_isError(result) && result == decompressedSpirvSize;
        if (!valid)
            cache.clear();
    });
    if (!valid)
        return nullptr;

    static std::mutex modulesMutex;
    static std::unordered_map<uint64_t, std::vector<uint32_t>> modules;
    std::lock_guard lock(modulesMutex);
    auto [it, inserted] = modules.try_emplace(entry.hash);
    if (inserted)
    {
        const uint8_t* encoded = cache.data() + entry.spirvOffset;
        const size_t decodedSize = smolv::GetDecodedBufferSize(encoded, entry.spirvSize);
        if (decodedSize == 0 || (decodedSize & 3u) != 0u)
            return nullptr;
        it->second.resize(decodedSize / sizeof(uint32_t));
        if (!smolv::Decode(encoded, entry.spirvSize, it->second.data(), decodedSize))
        {
            it->second.clear();
            return nullptr;
        }
    }
    if (it->second.empty() || it->second[0] != 0x07230203u)
        return nullptr;
    wordCount = it->second.size();
    return it->second.data();
}
