#pragma once

#include <cstddef>
#include <cstdint>

// This header is intentionally compatible with the source emitted by
// XenosRecomp. The generated cache is supplied at build time and remains
// outside the source repository.
struct ShaderCacheEntry
{
    uint64_t hash;
    uint32_t dxilOffset;
    uint32_t dxilSize;
    uint32_t spirvOffset;
    uint32_t spirvSize;
    uint32_t airOffset;
    uint32_t airSize;
    uint32_t specConstantsMask;
    const char* source;
};

struct ShaderMicrocodeEntry
{
    uint64_t microcodeHash;
    uint64_t shaderHash;
    uint32_t byteSize;
    uint32_t stage;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
extern ShaderMicrocodeEntry g_shaderMicrocodeEntries[];
extern const size_t g_shaderMicrocodeEntryCount;
