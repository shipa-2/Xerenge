#pragma once

#include "shader_cache.h"

struct XerengeShaderCacheView
{
    const ShaderCacheEntry* entries = nullptr;
    size_t entryCount = 0;
    const uint8_t* compressedSpirv = nullptr;
    size_t compressedSpirvSize = 0;
    size_t decompressedSpirvSize = 0;

    bool available() const { return entries != nullptr && entryCount != 0; }
    const ShaderCacheEntry* find(uint64_t hash) const;
};

XerengeShaderCacheView xerengeShaderCache();

