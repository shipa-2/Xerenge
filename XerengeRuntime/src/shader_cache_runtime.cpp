#include "shader_cache_runtime.h"

#include <algorithm>

#ifndef XERENGE_HAS_SHADER_CACHE
ShaderCacheEntry g_shaderCacheEntries[1]{};
const size_t g_shaderCacheEntryCount = 0;
const uint8_t g_compressedSpirvCache[1]{};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
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
