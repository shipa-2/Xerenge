#pragma once

#include <cstddef>
#include <cstdint>

// Compatible with XenosRecomp-generated shader_cache.cpp (see XerengeRuntime).
struct ShaderCacheEntry {
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

struct ShaderMicrocodeEntry {
  uint64_t microcodeHash;
  uint64_t shaderHash;
  uint32_t byteSize;
  uint32_t stage;
};

// fxc's DXBC, shader model 5.1 (tools/build_dxbc_cache.sh): D3D12 without DXIL, the
// Xbox One. One entry per shader and specialisation value it is compiled with.
struct DxbcCacheEntry {
  uint64_t hash;
  uint32_t specConstants;
  uint32_t specConstantsMask;
  const uint8_t* data;
  size_t size;
};
extern const DxbcCacheEntry g_dxbcCacheEntries[];
extern const size_t g_dxbcCacheEntryCount;

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
// The DXIL beside it, when XenosRecomp made some (on Windows): the D3D12 backend's.
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;
extern ShaderMicrocodeEntry g_shaderMicrocodeEntries[];
extern const size_t g_shaderMicrocodeEntryCount;
