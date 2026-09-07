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

// Runtime-captured shader caches can be linked alongside the main generated
// cache. The symbols are weak so builds without an auxiliary cache remain
// valid and simply use the primary cache.
#if defined(__GNUC__) || defined(__clang__)
#define XERENGE_WEAK __attribute__((weak))
#else
#define XERENGE_WEAK
#endif
extern ShaderCacheEntry g_shaderCacheEntriesExtra[] XERENGE_WEAK;
extern const size_t g_shaderCacheEntryCountExtra XERENGE_WEAK;
extern const uint8_t g_compressedSpirvCacheExtra[] XERENGE_WEAK;
extern const size_t g_spirvCacheCompressedSizeExtra XERENGE_WEAK;
extern const size_t g_spirvCacheDecompressedSizeExtra XERENGE_WEAK;
extern ShaderMicrocodeEntry g_shaderMicrocodeEntriesExtra[] XERENGE_WEAK;
extern const size_t g_shaderMicrocodeEntryCountExtra XERENGE_WEAK;

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
    if (available())
    {
        const auto* end = entries + entryCount;
        const auto* it = std::lower_bound(entries, end, hash,
            [](const ShaderCacheEntry& entry, uint64_t value) { return entry.hash < value; });
        if (it != end && it->hash == hash)
            return it;
    }
    if (&g_shaderCacheEntryCountExtra != nullptr && g_shaderCacheEntryCountExtra != 0)
    {
        const auto* begin = g_shaderCacheEntriesExtra;
        const auto* end = begin + g_shaderCacheEntryCountExtra;
        const auto* it = std::lower_bound(begin, end, hash,
            [](const ShaderCacheEntry& entry, uint64_t value) { return entry.hash < value; });
        if (it != end && it->hash == hash)
            return it;
    }
    return nullptr;
}

const ShaderMicrocodeEntry* XerengeShaderCacheView::findMicrocode(uint64_t hash) const
{
    auto search = [hash](const ShaderMicrocodeEntry* begin, size_t count) -> const ShaderMicrocodeEntry* {
        if (count == 0)
            return nullptr;
        const auto* end = begin + count;
        const auto* it = std::lower_bound(begin, end, hash,
            [](const ShaderMicrocodeEntry& entry, uint64_t value) {
                return entry.microcodeHash < value;
            });
        return it != end && it->microcodeHash == hash ? it : nullptr;
    };
    if (const auto* it = search(g_shaderMicrocodeEntries, g_shaderMicrocodeEntryCount))
        return it;
    if (&g_shaderMicrocodeEntryCountExtra != nullptr)
        if (const auto* it = search(g_shaderMicrocodeEntriesExtra, g_shaderMicrocodeEntryCountExtra))
            return it;

    // Burnout's bootstrap command stream emits a second 120-byte vertex
    // fetch variant whose stride words differ from the pointer shader already
    // present in the XenosRecomp cache. The generated SPIR-V is independent
    // of those fetch strides; the runtime supplies the current vertex binding
    // when it builds the Vulkan pipeline. Reuse the compiled module while
    // retaining the active microcode hash for pipeline state separation.
    if (hash == 0x1DB45A250C7CEE2Eull)
    {
        const auto* begin = g_shaderMicrocodeEntries;
        const auto* end = begin + g_shaderMicrocodeEntryCount;
        const auto* base = std::lower_bound(begin, end, 0xBCEC88072A5F344Dull,
            [](const ShaderMicrocodeEntry& entry, uint64_t value) {
                return entry.microcodeHash < value;
            });
        if (base != end && base->microcodeHash == 0xBCEC88072A5F344Dull)
        {
            static thread_local ShaderMicrocodeEntry alias;
            alias = *base;
            alias.microcodeHash = hash;
            return &alias;
        }
    }
    return nullptr;
}

const uint32_t* XerengeShaderCacheView::spirv(
    const ShaderCacheEntry& entry, size_t& wordCount) const
{
    wordCount = 0;
    const uint8_t* compressed = compressedSpirv;
    size_t compressedSize = compressedSpirvSize;
    size_t decompressedSize = decompressedSpirvSize;
    if (&g_shaderCacheEntryCountExtra != nullptr &&
        g_shaderCacheEntryCountExtra != 0 &&
        &entry >= g_shaderCacheEntriesExtra &&
        &entry < g_shaderCacheEntriesExtra + g_shaderCacheEntryCountExtra)
    {
        compressed = g_compressedSpirvCacheExtra;
        compressedSize = g_spirvCacheCompressedSizeExtra;
        decompressedSize = g_spirvCacheDecompressedSizeExtra;
    }
    if (compressed == nullptr || compressedSize == 0 || decompressedSize == 0 ||
        entry.spirvSize == 0 || entry.spirvOffset > decompressedSize ||
        entry.spirvSize > decompressedSize - entry.spirvOffset)
        return nullptr;

    const bool extra = compressed == g_compressedSpirvCacheExtra &&
        &g_shaderCacheEntryCountExtra != nullptr;
    static std::once_flag oncePrimary;
    static std::once_flag onceExtra;
    static std::vector<uint8_t> primaryCache;
    static std::vector<uint8_t> extraCache;
    static bool primaryValid = false;
    static bool extraValid = false;
    auto& once = extra ? onceExtra : oncePrimary;
    auto& cache = extra ? extraCache : primaryCache;
    auto& valid = extra ? extraValid : primaryValid;
    std::call_once(once, [&]
    {
        cache.resize(decompressedSize);
        const size_t result = ZSTD_decompress(cache.data(), decompressedSize,
            compressed, compressedSize);
        valid = !ZSTD_isError(result) && result == decompressedSize;
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
