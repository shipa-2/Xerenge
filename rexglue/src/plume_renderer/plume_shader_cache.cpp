/**
 * @file        plume_renderer/plume_shader_cache.cpp
 * @brief       XenosRecomp offline SPIR-V cache → plume::RenderShader (phase 2).
 */
#include "plume_renderer/plume_shader_cache.h"
#include "plume_renderer/plume_draw.h"
#include "plume_renderer/plume_dxc_link.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include <rex/logging.h>

#include "shader_cache.h"

#ifndef XERENGE_HAS_SHADER_CACHE
ShaderCacheEntry g_shaderCacheEntries[1]{};
const size_t g_shaderCacheEntryCount = 0;
const uint8_t g_compressedSpirvCache[1]{};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
ShaderMicrocodeEntry g_shaderMicrocodeEntries[1]{};
const size_t g_shaderMicrocodeEntryCount = 0;
#endif

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

extern ShaderCacheEntry g_shaderCacheEntriesBootstrap[] XERENGE_WEAK;
extern const size_t g_shaderCacheEntryCountBootstrap XERENGE_WEAK;
extern const uint8_t g_compressedSpirvCacheBootstrap[] XERENGE_WEAK;
extern const size_t g_spirvCacheCompressedSizeBootstrap XERENGE_WEAK;
extern const size_t g_spirvCacheDecompressedSizeBootstrap XERENGE_WEAK;
extern ShaderMicrocodeEntry g_shaderMicrocodeEntriesBootstrap[] XERENGE_WEAK;
extern const size_t g_shaderMicrocodeEntryCountBootstrap XERENGE_WEAK;

// DXIL, for the D3D12 backend (XERENGE_RENDER_API=d3d12): made beside the
// SPIR-V where XenosRecomp ran on Windows, absent elsewhere.
extern const uint8_t g_compressedDxilCache[] XERENGE_WEAK;
extern const size_t g_dxilCacheCompressedSize XERENGE_WEAK;
extern const size_t g_dxilCacheDecompressedSize XERENGE_WEAK;
extern const uint8_t g_compressedDxilCacheBootstrap[] XERENGE_WEAK;
extern const size_t g_dxilCacheCompressedSizeBootstrap XERENGE_WEAK;
extern const size_t g_dxilCacheDecompressedSizeBootstrap XERENGE_WEAK;
extern const uint8_t g_compressedDxilCacheExtra[] XERENGE_WEAK;
extern const size_t g_dxilCacheCompressedSizeExtra XERENGE_WEAK;
extern const size_t g_dxilCacheDecompressedSizeExtra XERENGE_WEAK;

namespace rex::plume_renderer {

namespace {

struct ShaderContainerHeader {
  uint32_t flags;
  uint32_t virtualSize;
  uint32_t physicalSize;
};

bool PrimaryAvailable() {
  return g_shaderCacheEntryCount > 0 && g_spirvCacheCompressedSize > 0 &&
         g_spirvCacheDecompressedSize > 0;
}

bool ExtraAvailable() {
  return &g_shaderCacheEntryCountExtra != nullptr && g_shaderCacheEntryCountExtra != 0 &&
         &g_spirvCacheCompressedSizeExtra != nullptr && g_spirvCacheCompressedSizeExtra != 0 &&
         &g_spirvCacheDecompressedSizeExtra != nullptr && g_spirvCacheDecompressedSizeExtra != 0;
}

bool BootstrapAvailable() {
  return &g_shaderCacheEntryCountBootstrap != nullptr && g_shaderCacheEntryCountBootstrap != 0 &&
         &g_spirvCacheCompressedSizeBootstrap != nullptr &&
         g_spirvCacheCompressedSizeBootstrap != 0 &&
         &g_spirvCacheDecompressedSizeBootstrap != nullptr &&
         g_spirvCacheDecompressedSizeBootstrap != 0;
}

const ShaderCacheEntry* LowerBoundHash(const ShaderCacheEntry* begin, size_t count, uint64_t hash) {
  if (!begin || count == 0) {
    return nullptr;
  }
  const ShaderCacheEntry* end = begin + count;
  const ShaderCacheEntry* it =
      std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& entry, uint64_t value) {
        return entry.hash < value;
      });
  return it != end && it->hash == hash ? it : nullptr;
}

const ShaderMicrocodeEntry* FindMicrocodeIn(const ShaderMicrocodeEntry* begin, size_t count,
                                            uint64_t hash) {
  if (!begin || count == 0) {
    return nullptr;
  }
  const ShaderMicrocodeEntry* end = begin + count;
  for (const ShaderMicrocodeEntry* it = begin; it != end; ++it) {
    if (it->microcodeHash == hash) {
      return it;
    }
  }
  return nullptr;
}

bool EntryIn(const ShaderCacheEntry& entry, const ShaderCacheEntry* begin, size_t count) {
  return begin && count != 0 && &entry >= begin && &entry < begin + count;
}

bool IsExtraEntry(const ShaderCacheEntry& entry) {
  return ExtraAvailable() &&
         EntryIn(entry, g_shaderCacheEntriesExtra, g_shaderCacheEntryCountExtra);
}

bool IsBootstrapEntry(const ShaderCacheEntry& entry) {
  return BootstrapAvailable() &&
         EntryIn(entry, g_shaderCacheEntriesBootstrap, g_shaderCacheEntryCountBootstrap);
}

// The translated vertex shaders write SV_ClipDistance for the title's clip
// plane. A GPU without shaderClipDistance (Mali) must not see it at all - its
// compiler crashed on such a shader and the frame never came. So for those the
// clip and cull distance outputs become private variables: still written, read
// by nothing, with the capability, their decorations and their place in the
// entry point's interface gone. Returns false when there was nothing to do.
bool StripClipDistance(const uint32_t* words, size_t count, std::vector<uint32_t>* out) {
  constexpr uint32_t kOpCapability = 17, kOpEntryPoint = 15, kOpDecorate = 71, kOpTypePointer = 32,
                     kOpVariable = 59, kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kCapClipDistance = 32, kCapCullDistance = 33, kDecorationBuiltIn = 11,
                     kBuiltInClipDistance = 3, kBuiltInCullDistance = 4, kStorageOutput = 3,
                     kStoragePrivate = 6;
  if (count < 5 || words[0] != 0x07230203u) {
    return false;
  }
  const auto each = [&](auto&& fn) {
    for (size_t i = 5; i < count;) {
      const uint32_t n = words[i] >> 16;
      if (n == 0 || i + n > count) {
        return false;
      }
      fn(i, words[i] & 0xFFFFu, n);
      i += n;
    }
    return true;
  };
  // The clip/cull variables, and the access chains into them.
  std::unordered_set<uint32_t> vars;
  if (!each([&](size_t i, uint32_t op, uint32_t n) {
        if (op == kOpDecorate && n >= 4 && words[i + 2] == kDecorationBuiltIn &&
            (words[i + 3] == kBuiltInClipDistance || words[i + 3] == kBuiltInCullDistance)) {
          vars.insert(words[i + 1]);
        }
      })) {
    return false;
  }
  if (vars.empty()) {
    return false;
  }
  std::unordered_set<uint32_t> chains;          // access chain results based on them
  std::unordered_set<uint32_t> output_pointers;  // the Output pointer types those use
  each([&](size_t i, uint32_t op, uint32_t n) {
    if (op == kOpVariable && n >= 4 && vars.count(words[i + 2])) {
      output_pointers.insert(words[i + 1]);
    } else if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && n >= 4 &&
               (vars.count(words[i + 3]) || chains.count(words[i + 3]))) {
      chains.insert(words[i + 2]);
      output_pointers.insert(words[i + 1]);
    }
  });
  // A Private twin for each of those pointer types, declared right after it.
  uint32_t bound = words[3];
  std::unordered_map<uint32_t, uint32_t> private_of;
  for (uint32_t p : output_pointers) {
    private_of[p] = bound++;
  }
  out->assign(words, words + 5);
  (*out)[3] = bound;
  each([&](size_t i, uint32_t op, uint32_t n) {
    const uint32_t* w = words + i;
    if (op == kOpCapability && n == 2 && (w[1] == kCapClipDistance || w[1] == kCapCullDistance)) {
      return;
    }
    if (op == kOpDecorate && n >= 3 && vars.count(w[1])) {
      return;
    }
    if (op == kOpEntryPoint && n >= 4) {
      // Model, function, the name (a string padded to whole words), then the interface.
      size_t at = 3;
      while (at < n) {
        const uint32_t word = w[at++];
        if ((word & 0xFF000000u) == 0 || (word & 0xFF0000u) == 0 || (word & 0xFF00u) == 0 ||
            (word & 0xFFu) == 0) {
          break;
        }
      }
      std::vector<uint32_t> inst(w, w + at);
      for (size_t k = at; k < n; ++k) {
        if (!vars.count(w[k])) {
          inst.push_back(w[k]);
        }
      }
      inst[0] = (uint32_t(inst.size()) << 16) | kOpEntryPoint;
      out->insert(out->end(), inst.begin(), inst.end());
      return;
    }
    if (op == kOpVariable && n >= 4 && vars.count(w[2])) {
      out->insert(out->end(), w, w + n);
      out->at(out->size() - n + 1) = private_of[w[1]];
      out->at(out->size() - n + 3) = kStoragePrivate;
      return;
    }
    if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && n >= 4 && chains.count(w[2])) {
      out->insert(out->end(), w, w + n);
      out->at(out->size() - n + 1) = private_of[w[1]];
      return;
    }
    out->insert(out->end(), w, w + n);
    if (op == kOpTypePointer && n == 4 && private_of.count(w[1])) {
      out->insert(out->end(), {(4u << 16) | kOpTypePointer, private_of[w[1]], kStoragePrivate, w[3]});
    }
  });
  return true;
}

}  // namespace

PlumeShaderCache& PlumeShaderCache::Instance() {
  static PlumeShaderCache cache;
  return cache;
}

void PlumeShaderCache::SetDevice(plume::RenderDevice* device) {
  std::lock_guard lock(mutex_);
  device_ = device;
}

bool PlumeShaderCache::IsAvailable() const {
  return PrimaryAvailable() || ExtraAvailable() || BootstrapAvailable();
}

size_t PlumeShaderCache::EntryCount() const {
  return g_shaderCacheEntryCount + (ExtraAvailable() ? g_shaderCacheEntryCountExtra : 0) +
         (BootstrapAvailable() ? g_shaderCacheEntryCountBootstrap : 0);
}

uint64_t PlumeShaderCache::HashShaderContainer(const void* shader_container) const {
  if (!shader_container) {
    return 0;
  }
  const auto* container = static_cast<const ShaderContainerHeader*>(shader_container);
  const uint32_t hash_len = container->virtualSize + container->physicalSize;
  if (hash_len == 0) {
    return 0;
  }
  return XXH3_64bits(shader_container, hash_len);
}

const ShaderCacheEntry* PlumeShaderCache::Find(uint64_t hash) const {
  if (const ShaderCacheEntry* it = LowerBoundHash(g_shaderCacheEntries, g_shaderCacheEntryCount, hash)) {
    return it;
  }
  if (ExtraAvailable()) {
    if (const ShaderCacheEntry* it =
            LowerBoundHash(g_shaderCacheEntriesExtra, g_shaderCacheEntryCountExtra, hash)) {
      return it;
    }
  }
  if (BootstrapAvailable()) {
    return LowerBoundHash(g_shaderCacheEntriesBootstrap, g_shaderCacheEntryCountBootstrap, hash);
  }
  return nullptr;
}

const std::vector<uint32_t>& PlumeShaderCache::MicrocodeSizes(uint32_t stage) {
  std::lock_guard lock(mutex_);
  if (!microcode_sizes_built_) {
    microcode_sizes_built_ = true;
    auto take = [&](const ShaderMicrocodeEntry* entries, size_t count) {
      if (!entries) {
        return;
      }
      for (size_t i = 0; i < count; ++i) {
        const uint32_t s = entries[i].stage ? 1u : 0u;
        if (entries[i].byteSize != 0) {
          microcode_sizes_[s].push_back(entries[i].byteSize);
        }
      }
    };
    take(g_shaderMicrocodeEntries, g_shaderMicrocodeEntryCount);
    if (ExtraAvailable()) {
      take(g_shaderMicrocodeEntriesExtra, g_shaderMicrocodeEntryCountExtra);
    }
    if (BootstrapAvailable()) {
      take(g_shaderMicrocodeEntriesBootstrap, g_shaderMicrocodeEntryCountBootstrap);
    }
    for (auto& sizes : microcode_sizes_) {
      std::sort(sizes.begin(), sizes.end());
      sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    }
  }
  return microcode_sizes_[stage ? 1 : 0];
}

const ShaderMicrocodeEntry* PlumeShaderCache::FindByMicrocode(uint64_t microcode_hash) const {
  auto find_micro = [&](uint64_t hash) -> const ShaderMicrocodeEntry* {
    if (const ShaderMicrocodeEntry* it =
            FindMicrocodeIn(g_shaderMicrocodeEntries, g_shaderMicrocodeEntryCount, hash)) {
      return it;
    }
    // The runtime cache (bootstrap slot) before the extra one: it is rebuilt
    // from the disc with its HLSL kept, so its shaders are in the source-info
    // table. The extra cache's hand-wrapped ones are not - the interface panel
    // VS among them lost its position scaling and was converted twice.
    if (BootstrapAvailable()) {
      if (const ShaderMicrocodeEntry* it = FindMicrocodeIn(
              g_shaderMicrocodeEntriesBootstrap, g_shaderMicrocodeEntryCountBootstrap, hash)) {
        return it;
      }
    }
    if (ExtraAvailable()) {
      if (const ShaderMicrocodeEntry* it = FindMicrocodeIn(
              g_shaderMicrocodeEntriesExtra, g_shaderMicrocodeEntryCountExtra, hash)) {
        return it;
      }
    }
    return nullptr;
  };

  if (const ShaderMicrocodeEntry* it = find_micro(microcode_hash)) {
    return it;
  }

  // Bootstrap command stream emits a 120-byte vertex-fetch variant whose
  // stride words differ from the pointer shader already in the cache. SPIR-V
  // is independent of those strides.
  if (microcode_hash == 0x1DB45A250C7CEE2Eull) {
    if (const ShaderMicrocodeEntry* base = find_micro(0xBCEC88072A5F344Dull)) {
      static thread_local ShaderMicrocodeEntry alias;
      alias = *base;
      alias.microcodeHash = microcode_hash;
      return &alias;
    }
  }
  return nullptr;
}

bool PlumeShaderCache::EnsureDecompressed() {
  if (decompressed_valid_) {
    return true;
  }
  if (!PrimaryAvailable()) {
    return false;
  }

  decompressed_spirv_.resize(g_spirvCacheDecompressedSize);
  const size_t result =
      ZSTD_decompress(decompressed_spirv_.data(), g_spirvCacheDecompressedSize,
                      g_compressedSpirvCache, g_spirvCacheCompressedSize);
  decompressed_valid_ = !ZSTD_isError(result) && result == g_spirvCacheDecompressedSize;
  if (!decompressed_valid_) {
    decompressed_spirv_.clear();
    REXLOG_ERROR("plume: SPIR-V cache ZSTD decompress failed ({})", ZSTD_getErrorName(result));
  }
  return decompressed_valid_;
}

bool PlumeShaderCache::EnsureExtraDecompressed() {
  if (extra_decompressed_valid_) {
    return true;
  }
  if (!ExtraAvailable()) {
    return false;
  }

  extra_decompressed_spirv_.resize(g_spirvCacheDecompressedSizeExtra);
  const size_t result =
      ZSTD_decompress(extra_decompressed_spirv_.data(), g_spirvCacheDecompressedSizeExtra,
                      g_compressedSpirvCacheExtra, g_spirvCacheCompressedSizeExtra);
  extra_decompressed_valid_ = !ZSTD_isError(result) && result == g_spirvCacheDecompressedSizeExtra;
  if (!extra_decompressed_valid_) {
    extra_decompressed_spirv_.clear();
    REXLOG_ERROR("plume: extra SPIR-V cache ZSTD decompress failed ({})",
                 ZSTD_getErrorName(result));
  }
  return extra_decompressed_valid_;
}

bool PlumeShaderCache::EnsureBootstrapDecompressed() {
  if (bootstrap_decompressed_valid_) {
    return true;
  }
  if (!BootstrapAvailable()) {
    return false;
  }

  bootstrap_decompressed_spirv_.resize(g_spirvCacheDecompressedSizeBootstrap);
  const size_t result =
      ZSTD_decompress(bootstrap_decompressed_spirv_.data(), g_spirvCacheDecompressedSizeBootstrap,
                      g_compressedSpirvCacheBootstrap, g_spirvCacheCompressedSizeBootstrap);
  bootstrap_decompressed_valid_ =
      !ZSTD_isError(result) && result == g_spirvCacheDecompressedSizeBootstrap;
  if (!bootstrap_decompressed_valid_) {
    bootstrap_decompressed_spirv_.clear();
    REXLOG_ERROR("plume: bootstrap SPIR-V cache ZSTD decompress failed ({})",
                 ZSTD_getErrorName(result));
  }
  return bootstrap_decompressed_valid_;
}

const uint32_t* PlumeShaderCache::GetSpirvWords(const ShaderCacheEntry& entry,
                                                size_t& word_count) {
  word_count = 0;
  const bool extra = IsExtraEntry(entry);
  const bool bootstrap = !extra && IsBootstrapEntry(entry);
  if (extra) {
    if (!EnsureExtraDecompressed()) {
      return nullptr;
    }
  } else if (bootstrap) {
    if (!EnsureBootstrapDecompressed()) {
      return nullptr;
    }
  } else if (!EnsureDecompressed()) {
    return nullptr;
  }

  std::vector<uint8_t>& blob = extra ? extra_decompressed_spirv_
                               : bootstrap ? bootstrap_decompressed_spirv_
                                           : decompressed_spirv_;
  if (entry.spirvSize == 0 || entry.spirvOffset > blob.size() ||
      entry.spirvSize > blob.size() - entry.spirvOffset) {
    return nullptr;
  }

  auto [it, inserted] = spirv_modules_.try_emplace(entry.hash);
  if (inserted) {
    const uint8_t* encoded = blob.data() + entry.spirvOffset;
    const size_t decoded_size = smolv::GetDecodedBufferSize(encoded, entry.spirvSize);
    if (decoded_size == 0 || (decoded_size & 3u) != 0u) {
      REXLOG_ERROR("plume: invalid smol-v size for shader {:016X}", entry.hash);
      return nullptr;
    }
    it->second.resize(decoded_size / sizeof(uint32_t));
    if (!smolv::Decode(encoded, entry.spirvSize, it->second.data(), decoded_size)) {
      it->second.clear();
      REXLOG_ERROR("plume: smol-v decode failed for shader {:016X}", entry.hash);
      return nullptr;
    }
  }

  if (it->second.empty() || it->second[0] != 0x07230203u) {
    return nullptr;
  }

  word_count = it->second.size();
  return it->second.data();
}

// A pixel shader that only writes red, put in place of the shaders listed in
// XERENGE_RED_PS (hex hashes, comma separated): a draw that then shows red
// reaches the screen and its own shader is what fails; one that does not is
// lost before shading. For a GPU without RenderDoc (old Adreno on a phone).
// glslangValidator -V --target-env vulkan1.0 -e shaderMain of
// "layout(location = 0) out vec4 colour; void main() { colour = vec4(1, 0, 0, 1); }".
static const uint32_t kRedPsSpirv[] = {0x07230203u, 0x00010000u, 0x0008000Bu, 0x0000000Du, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000Bu, 0x00000001u, 0x4C534C47u, 0x6474732Eu, 0x3035342Eu, 0x00000000u, 0x0003000Eu, 0x00000000u, 0x00000001u, 0x0007000Fu, 0x00000004u, 0x00000004u, 0x64616873u, 0x614D7265u, 0x00006E69u, 0x00000009u, 0x00030010u, 0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001C2u, 0x00050005u, 0x00000004u, 0x64616873u, 0x614D7265u, 0x00006E69u, 0x00040005u, 0x00000009u, 0x6F6C6F63u, 0x00007275u, 0x00040047u, 0x00000009u, 0x0000001Eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003Bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x0004002Bu, 0x00000006u, 0x0000000Au, 0x3F800000u, 0x0004002Bu, 0x00000006u, 0x0000000Bu, 0x00000000u, 0x0007002Cu, 0x00000007u, 0x0000000Cu, 0x0000000Au, 0x0000000Bu, 0x0000000Bu, 0x0000000Au, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200F8u, 0x00000005u, 0x0003003Eu, 0x00000009u, 0x0000000Cu, 0x000100FDu, 0x00010038u};

static std::vector<uint64_t> ListedHashes(const char* variable) {
    std::vector<uint64_t> hashes;
    if (const char* list = std::getenv(variable)) {
      for (const char* p = list; *p;) {
        char* end = nullptr;
        const uint64_t value = std::strtoull(p, &end, 16);
        if (end == p) {
          break;
        }
        // Eight digits or fewer: the top half of the hash (a property on Android 10
        // holds 91 characters, not five whole hashes).
        hashes.push_back(end - p <= 8 ? (value << 32) | 0xFFFFFFFFull : value);
        p = *end == ',' ? end + 1 : end;
      }
    }
    return hashes;
}

static bool Listed(const std::vector<uint64_t>& listed, uint64_t hash) {
  return std::any_of(listed.begin(), listed.end(), [hash](uint64_t h) {
    return (h & 0xFFFFFFFFull) == 0xFFFFFFFFull ? (h >> 32) == (hash >> 32) : h == hash;
  });
}

// A vertex shader's inputs numbered 0, 1, 2... in the order of their
// locations: the title's go as high as 17, a Mali-G71 takes locations below 16
// only (maxVertexInputAttributes), and its GPU hung at the first frame. The
// pipeline takes each input at its rank (GetOrCreatePipeline); the location
// mask (InputLocationMask) is still read from the shader as translated.
// Returns false when there is nothing to change.
static bool CompactInputLocations(const uint32_t* words, size_t count, std::vector<uint32_t>* out) {
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59;
  constexpr uint32_t kDecorationLocation = 30, kStorageClassInput = 1, kExecutionModelVertex = 0;
  bool vertex = false;
  std::unordered_map<uint32_t, size_t> location_word;  // id -> index of its location operand
  std::vector<uint32_t> inputs;
  for (size_t i = 5; i < count;) {
    const uint32_t length = words[i] >> 16, op = words[i] & 0xFFFFu;
    if (length == 0 || i + length > count) {
      return false;
    }
    if (op == kOpEntryPoint && length >= 2) {
      vertex = vertex || words[i + 1] == kExecutionModelVertex;
    } else if (op == kOpDecorate && length >= 4 && words[i + 2] == kDecorationLocation) {
      location_word[words[i + 1]] = i + 3;
    } else if (op == kOpVariable && length >= 4 && words[i + 3] == kStorageClassInput) {
      inputs.push_back(words[i + 2]);
    }
    i += length;
  }
  if (!vertex) {
    return false;
  }
  std::vector<std::pair<uint32_t, size_t>> placed;  // location, word index
  for (const uint32_t id : inputs) {
    if (auto at = location_word.find(id); at != location_word.end()) {
      placed.emplace_back(words[at->second], at->second);
    }
  }
  std::sort(placed.begin(), placed.end());
  bool changed = false;
  for (size_t rank = 0; rank < placed.size(); ++rank) {
    changed = changed || placed[rank].first != rank;
  }
  if (!changed) {
    return false;
  }
  out->assign(words, words + count);
  for (size_t rank = 0; rank < placed.size(); ++rank) {
    (*out)[placed[rank].second] = uint32_t(rank);
  }
  return true;
}

static bool RedPixelShader(uint64_t hash) {
  static const std::vector<uint64_t> listed = ListedHashes("XERENGE_RED_PS");
  return Listed(listed, hash);
}

// And a vertex shader that only places the vertex - position from location 1
// through c0..c3, Y flipped as the translated ones do - in place of those in
// XERENGE_SIMPLE_VS: what the title's shader computes besides, taken out.
static const uint32_t kSimpleVsSpirv[] = {0x07230203u, 0x00010000u, 0x0008000Bu, 0x00000039u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000Bu, 0x00000001u, 0x4C534C47u, 0x6474732Eu, 0x3035342Eu, 0x00000000u, 0x0003000Eu, 0x00000000u, 0x00000001u, 0x0008000Fu, 0x00000000u, 0x00000004u, 0x64616873u, 0x614D7265u, 0x00006E69u, 0x0000000Bu, 0x00000019u, 0x00030003u, 0x00000002u, 0x000001C2u, 0x00050005u, 0x00000004u, 0x64616873u, 0x614D7265u, 0x00006E69u, 0x00030005u, 0x00000009u, 0x00000070u, 0x00050005u, 0x0000000Bu, 0x69736F70u, 0x6E6F6974u, 0x00000000u, 0x00060005u, 0x00000017u, 0x505F6C67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x00000017u, 0x00000000u, 0x505F6C67u, 0x7469736Fu, 0x006E6F69u, 0x00070006u, 0x00000017u, 0x00000001u, 0x505F6C67u, 0x746E696Fu, 0x657A6953u, 0x00000000u, 0x00070006u, 0x00000017u, 0x00000002u, 0x435F6C67u, 0x4470696Cu, 0x61747369u, 0x0065636Eu, 0x00070006u, 0x00000017u, 0x00000003u, 0x435F6C67u, 0x446C6C75u, 0x61747369u, 0x0065636Eu, 0x00030005u, 0x00000019u, 0x00000000u, 0x00060005u, 0x0000001Eu, 0x74726556u, 0x6F437865u, 0x6174736Eu, 0x0073746Eu, 0x00040006u, 0x0000001Eu, 0x00000000u, 0x00000063u, 0x00030005u, 0x00000020u, 0x00007376u, 0x00040047u, 0x0000000Bu, 0x0000001Eu, 0x00000001u, 0x00030047u, 0x00000017u, 0x00000002u, 0x00050048u, 0x00000017u, 0x00000000u, 0x0000000Bu, 0x00000000u, 0x00050048u, 0x00000017u, 0x00000001u, 0x0000000Bu, 0x00000001u, 0x00050048u, 0x00000017u, 0x00000002u, 0x0000000Bu, 0x00000003u, 0x00050048u, 0x00000017u, 0x00000003u, 0x0000000Bu, 0x00000004u, 0x00040047u, 0x0000001Du, 0x00000006u, 0x00000010u, 0x00030047u, 0x0000001Eu, 0x00000002u, 0x00050048u, 0x0000001Eu, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000020u, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000020u, 0x00000022u, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000007u, 0x00000007u, 0x00040020u, 0x0000000Au, 0x00000001u, 0x00000007u, 0x0004003Bu, 0x0000000Au, 0x0000000Bu, 0x00000001u, 0x00040017u, 0x0000000Cu, 0x00000006u, 0x00000003u, 0x0004002Bu, 0x00000006u, 0x0000000Fu, 0x3F800000u, 0x00040015u, 0x00000014u, 0x00000020u, 0x00000000u, 0x0004002Bu, 0x00000014u, 0x00000015u, 0x00000001u, 0x0004001Cu, 0x00000016u, 0x00000006u, 0x00000015u, 0x0006001Eu, 0x00000017u, 0x00000007u, 0x00000006u, 0x00000016u, 0x00000016u, 0x00040020u, 0x00000018u, 0x00000003u, 0x00000017u, 0x0004003Bu, 0x00000018u, 0x00000019u, 0x00000003u, 0x00040015u, 0x0000001Au, 0x00000020u, 0x00000001u, 0x0004002Bu, 0x0000001Au, 0x0000001Bu, 0x00000000u, 0x0004002Bu, 0x00000014u, 0x0000001Cu, 0x00000100u, 0x0004001Cu, 0x0000001Du, 0x00000007u, 0x0000001Cu, 0x0003001Eu, 0x0000001Eu, 0x0000001Du, 0x00040020u, 0x0000001Fu, 0x00000002u, 0x0000001Eu, 0x0004003Bu, 0x0000001Fu, 0x00000020u, 0x00000002u, 0x00040020u, 0x00000021u, 0x00000002u, 0x00000007u, 0x0004002Bu, 0x0000001Au, 0x00000026u, 0x00000001u, 0x0004002Bu, 0x0000001Au, 0x0000002Cu, 0x00000002u, 0x0004002Bu, 0x0000001Au, 0x00000031u, 0x00000003u, 0x00040020u, 0x00000037u, 0x00000003u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200F8u, 0x00000005u, 0x0004003Bu, 0x00000008u, 0x00000009u, 0x00000007u, 0x0004003Du, 0x00000007u, 0x0000000Du, 0x0000000Bu, 0x0008004Fu, 0x0000000Cu, 0x0000000Eu, 0x0000000Du, 0x0000000Du, 0x00000000u, 0x00000001u, 0x00000002u, 0x00050051u, 0x00000006u, 0x00000010u, 0x0000000Eu, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000011u, 0x0000000Eu, 0x00000001u, 0x00050051u, 0x00000006u, 0x00000012u, 0x0000000Eu, 0x00000002u, 0x00070050u, 0x00000007u, 0x00000013u, 0x00000010u, 0x00000011u, 0x00000012u, 0x0000000Fu, 0x0003003Eu, 0x00000009u, 0x00000013u, 0x00060041u, 0x00000021u, 0x00000022u, 0x00000020u, 0x0000001Bu, 0x0000001Bu, 0x0004003Du, 0x00000007u, 0x00000023u, 0x00000022u, 0x0004003Du, 0x00000007u, 0x00000024u, 0x00000009u, 0x00050094u, 0x00000006u, 0x00000025u, 0x00000023u, 0x00000024u, 0x00060041u, 0x00000021u, 0x00000027u, 0x00000020u, 0x0000001Bu, 0x00000026u, 0x0004003Du, 0x00000007u, 0x00000028u, 0x00000027u, 0x0004003Du, 0x00000007u, 0x00000029u, 0x00000009u, 0x00050094u, 0x00000006u, 0x0000002Au, 0x00000028u, 0x00000029u, 0x0004007Fu, 0x00000006u, 0x0000002Bu, 0x0000002Au, 0x00060041u, 0x00000021u, 0x0000002Du, 0x00000020u, 0x0000001Bu, 0x0000002Cu, 0x0004003Du, 0x00000007u, 0x0000002Eu, 0x0000002Du, 0x0004003Du, 0x00000007u, 0x0000002Fu, 0x00000009u, 0x00050094u, 0x00000006u, 0x00000030u, 0x0000002Eu, 0x0000002Fu, 0x00060041u, 0x00000021u, 0x00000032u, 0x00000020u, 0x0000001Bu, 0x00000031u, 0x0004003Du, 0x00000007u, 0x00000033u, 0x00000032u, 0x0004003Du, 0x00000007u, 0x00000034u, 0x00000009u, 0x00050094u, 0x00000006u, 0x00000035u, 0x00000033u, 0x00000034u, 0x00070050u, 0x00000007u, 0x00000036u, 0x00000025u, 0x0000002Bu, 0x00000030u, 0x00000035u, 0x00050041u, 0x00000037u, 0x00000038u, 0x00000019u, 0x0000001Bu, 0x0003003Eu, 0x00000038u, 0x00000036u, 0x000100FDu, 0x00010038u};

static bool SimpleVertexShader(uint64_t hash) {
  static const std::vector<uint64_t> listed = ListedHashes("XERENGE_SIMPLE_VS");
  return Listed(listed, hash);
}

const uint8_t* PlumeShaderCache::GetDxil(const ShaderCacheEntry& entry, size_t& size) {
  size = 0;
  const bool extra = IsExtraEntry(entry);
  const bool bootstrap = !extra && IsBootstrapEntry(entry);
  const int which = extra ? 2 : bootstrap ? 1 : 0;
  std::vector<uint8_t>& blob = dxil_blobs_[which];
  if (!dxil_tried_[which]) {
    dxil_tried_[which] = true;
    const uint8_t* compressed = nullptr;
    const size_t* compressed_size = nullptr;
    const size_t* decompressed_size = nullptr;
    if (which == 0) {
      compressed = g_compressedDxilCache;
      compressed_size = &g_dxilCacheCompressedSize;
      decompressed_size = &g_dxilCacheDecompressedSize;
    } else if (which == 1) {
      compressed = g_compressedDxilCacheBootstrap;
      compressed_size = &g_dxilCacheCompressedSizeBootstrap;
      decompressed_size = &g_dxilCacheDecompressedSizeBootstrap;
    } else {
      compressed = g_compressedDxilCacheExtra;
      compressed_size = &g_dxilCacheCompressedSizeExtra;
      decompressed_size = &g_dxilCacheDecompressedSizeExtra;
    }
    if (compressed && compressed_size && decompressed_size && *compressed_size != 0 &&
        *decompressed_size != 0) {
      blob.resize(*decompressed_size);
      const size_t result = ZSTD_decompress(blob.data(), blob.size(), compressed, *compressed_size);
      if (ZSTD_isError(result) || result != blob.size()) {
        REXLOG_ERROR("plume: DXIL cache {} ZSTD decompress failed", which);
        blob.clear();
      }
    }
    REXLOG_INFO("plume: DXIL cache {}: {} KiB", which == 0 ? "primary" : which == 1 ? "bootstrap" : "extra",
                blob.size() / 1024);
  }
  if (entry.dxilSize == 0 || entry.dxilOffset > blob.size() ||
      entry.dxilSize > blob.size() - entry.dxilOffset) {
    return nullptr;
  }
  size = entry.dxilSize;
  return blob.data() + entry.dxilOffset;
}

plume::RenderShader* PlumeShaderCache::GetOrCreateShader(uint64_t hash, uint32_t spec_constants,
                                                       bool pixel_shader) {
  std::lock_guard lock(mutex_);
  if (!device_) {
    return nullptr;
  }

  // D3D12: a shader with specialisation constants is a DXIL library calling
  // g_SpecConstants(); each value it is used with is linked into a shader of
  // its own.
  const ShaderCacheEntry* linked_entry = nullptr;
  uint32_t spec = 0;
  if (!PlumeUsesVulkan()) {
    linked_entry = Find(hash);
    if (linked_entry && linked_entry->specConstantsMask != 0) {
      spec = spec_constants & linked_entry->specConstantsMask;
      const uint64_t key = hash ^ (uint64_t(spec) << 32) ^ (pixel_shader ? 1ull : 0ull);
      if (auto it = linked_shaders_.find(key); it != linked_shaders_.end()) {
        return it->second.get();
      }
      size_t size = 0;
      const uint8_t* library = GetDxil(*linked_entry, size);
      std::vector<uint8_t> linked = library ? LinkSpecConstants(library, size, pixel_shader, spec)
                                            : std::vector<uint8_t>{};
      std::unique_ptr<plume::RenderShader> shader;
      if (!linked.empty()) {
        shader = device_->createShader(linked.data(), linked.size(), "shaderMain",
                                       plume::RenderShaderFormat::DXIL);
      } else {
        static std::atomic<uint32_t> failed{0};
        if (failed.fetch_add(1, std::memory_order_relaxed) < 8) {
          REXLOG_ERROR("plume: DXIL link failed for shader {:016X} (spec {:X}{})", hash, spec,
                       library ? "" : ", no DXIL");
        }
      }
      plume::RenderShader* raw = shader.get();
      linked_shaders_.emplace(key, std::move(shader));
      return raw;
    }
  }

  if (auto it = shaders_.find(hash); it != shaders_.end()) {
    return it->second.get();
  }

  const ShaderCacheEntry* entry = Find(hash);
  if (!entry) {
    return nullptr;
  }

  // D3D12: the DXIL XenosRecomp made beside the SPIR-V, as it is.
  if (!PlumeUsesVulkan()) {
    size_t size = 0;
    const uint8_t* dxil = GetDxil(*entry, size);
    if (!dxil) {
      static std::atomic<uint32_t> missing{0};
      if (missing.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_ERROR("plume: no DXIL for shader {:016X}", hash);
      }
      return nullptr;
    }
    std::unique_ptr<plume::RenderShader> shader =
        device_->createShader(dxil, size, "shaderMain", plume::RenderShaderFormat::DXIL);
    if (!shader) {
      REXLOG_ERROR("plume: createShader (DXIL) failed for cache entry {:016X}", hash);
      return nullptr;
    }
    plume::RenderShader* raw = shader.get();
    shaders_.emplace(hash, std::move(shader));
    return raw;
  }

  size_t word_count = 0;
  const uint32_t* words = GetSpirvWords(*entry, word_count);
  if (!words || word_count == 0) {
    return nullptr;
  }

  // No SV_ClipDistance for a GPU that cannot take it (see StripClipDistance).
  // XERENGE_NO_CLIP_DISTANCE=1 strips it anywhere, for telling a driver's
  // clip distances from the rest.
  static const bool no_clip = std::getenv("XERENGE_NO_CLIP_DISTANCE") != nullptr;
  std::vector<uint32_t> stripped;
  if ((no_clip || !device_->getCapabilities().clipDistance) && StripClipDistance(words, word_count, &stripped)) {
    words = stripped.data();
    word_count = stripped.size();
  }
  // Capabilities beyond the plain shader ones, named once per shader: a GPU
  // that lacks one (Int64, Float16 and the like on Adreno 5xx and old Mali)
  // runs the shader wrongly rather than refusing it.
  {
    std::string unusual;
    for (size_t i = 5; i < word_count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0) {
        break;
      }
      if (opcode == 17 && i + 1 < word_count) {  // OpCapability
        const uint32_t cap = words[i + 1];
        // Matrix, Shader, ClipDistance, CullDistance, ImageQuery, DerivativeControl
        if (cap != 0 && cap != 1 && cap != 32 && cap != 33 && cap != 50 && cap != 51) {
          unusual += fmt::format(" {}", cap);
        }
      } else if (opcode != 17 && opcode != 11 && opcode != 14 && opcode != 15 && opcode != 16 &&
                 opcode != 10 && opcode != 3 && opcode != 5 && opcode != 6 && opcode != 7) {
        break;  // past the header's instructions
      }
      i += length;
    }
    if (!unusual.empty()) {
      REXLOG_INFO("plume: shader {:016X} needs capabilities{}", hash, unusual);
    }
  }
  // XERENGE_DUMP_SPIRV=<first hex digits of a hash>: that shader's SPIR-V as
  // spirv_<hash>.spv in the working directory, for spirv-dis.
  if (const char* dump = std::getenv("XERENGE_DUMP_SPIRV");
      dump && *dump && fmt::format("{:016X}", hash).rfind(dump, 0) == 0) {
    if (FILE* f = std::fopen(fmt::format("spirv_{:016X}.spv", hash).c_str(), "wb")) {
      std::fwrite(words, sizeof(uint32_t), word_count, f);
      std::fclose(f);
    }
  }
  if (RedPixelShader(hash)) {
    words = kRedPsSpirv;
    word_count = std::size(kRedPsSpirv);
    REXLOG_INFO("plume: pixel shader {:016X} drawn plain red (XERENGE_RED_PS)", hash);
  }
  if (SimpleVertexShader(hash)) {
    words = kSimpleVsSpirv;
    word_count = std::size(kSimpleVsSpirv);
    REXLOG_INFO("plume: vertex shader {:016X} replaced by a position-only one (XERENGE_SIMPLE_VS)", hash);
  }
  // Last, so a stand-in (XERENGE_SIMPLE_VS) is numbered the same way.
  std::vector<uint32_t> compacted;
  if (CompactInputLocations(words, word_count, &compacted)) {
    words = compacted.data();
    word_count = compacted.size();
  }
  std::unique_ptr<plume::RenderShader> shader = device_->createShader(
      words, word_count * sizeof(uint32_t), "shaderMain", plume::RenderShaderFormat::SPIRV);
  if (!shader) {
    REXLOG_ERROR("plume: createShader failed for cache entry {:016X}", hash);
    return nullptr;
  }

  plume::RenderShader* raw = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return raw;
}

uint32_t PlumeShaderCache::InputLocationMask(uint64_t hash) {
  std::lock_guard lock(mutex_);
  if (auto it = input_masks_.find(hash); it != input_masks_.end()) {
    return it->second;
  }
  uint32_t mask = 0;
  const ShaderCacheEntry* entry = Find(hash);
  size_t word_count = 0;
  const uint32_t* words = entry ? GetSpirvWords(*entry, word_count) : nullptr;
  if (words && word_count > 5) {
    // OpDecorate Location / BuiltIn, then the Input variables they name.
    constexpr uint32_t kOpDecorate = 71;
    constexpr uint32_t kOpVariable = 59;
    constexpr uint32_t kDecorationBuiltIn = 11;
    constexpr uint32_t kDecorationLocation = 30;
    constexpr uint32_t kStorageClassInput = 1;
    std::unordered_map<uint32_t, uint32_t> locations;
    std::vector<uint32_t> inputs;
    for (size_t i = 5; i < word_count;) {
      const uint32_t count = words[i] >> 16;
      const uint32_t op = words[i] & 0xFFFFu;
      if (count == 0 || i + count > word_count) {
        break;
      }
      if (op == kOpDecorate && count >= 4 && words[i + 2] == kDecorationLocation) {
        locations.emplace(words[i + 1], words[i + 3]);
      } else if (op == kOpDecorate && count >= 3 && words[i + 2] == kDecorationBuiltIn) {
        locations[words[i + 1]] = ~0u;  // never a location of its own
      } else if (op == kOpVariable && count >= 4 && words[i + 3] == kStorageClassInput) {
        inputs.push_back(words[i + 2]);
      }
      i += count;
    }
    for (const uint32_t id : inputs) {
      if (auto at = locations.find(id); at != locations.end() && at->second < 32) {
        mask |= 1u << at->second;
      }
    }
  }
  input_masks_.emplace(hash, mask);
  return mask;
}

void PlumeShaderCache::WarmAll() {
  size_t ok = 0;
  size_t fail = 0;
  auto warm = [&](const ShaderCacheEntry* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      // D3D12: shaders with specialisation constants are linked per pipeline.
      if (!PlumeUsesVulkan() && entries[i].specConstantsMask != 0) {
        continue;
      }
      if (GetOrCreateShader(entries[i].hash)) {
        ++ok;
      } else {
        ++fail;
      }
    }
  };
  warm(g_shaderCacheEntries, g_shaderCacheEntryCount);
  if (ExtraAvailable()) {
    warm(g_shaderCacheEntriesExtra, g_shaderCacheEntryCountExtra);
  }
  if (BootstrapAvailable()) {
    warm(g_shaderCacheEntriesBootstrap, g_shaderCacheEntryCountBootstrap);
  }
  REXLOG_INFO("plume: warmed {} host shaders ({} failed)", ok, fail);
}

void PlumeShaderCache::LogSummary() const {
  if (!IsAvailable()) {
    REXLOG_INFO("plume: no offline shader cache linked (XenosRecomp output missing)");
    return;
  }

  REXLOG_INFO("plume: offline shader cache ready ({} entries, {} microcode, {} KiB SPIR-V)",
              g_shaderCacheEntryCount, g_shaderMicrocodeEntryCount,
              g_spirvCacheDecompressedSize / 1024);
  if (ExtraAvailable()) {
    REXLOG_INFO("plume: extra shader cache ready ({} entries, {} microcode, {} KiB SPIR-V)",
                g_shaderCacheEntryCountExtra, g_shaderMicrocodeEntryCountExtra,
                g_spirvCacheDecompressedSizeExtra / 1024);
  }
  if (BootstrapAvailable()) {
    REXLOG_INFO("plume: bootstrap shader cache ready ({} entries, {} microcode, {} KiB SPIR-V)",
                g_shaderCacheEntryCountBootstrap, g_shaderMicrocodeEntryCountBootstrap,
                g_spirvCacheDecompressedSizeBootstrap / 1024);
  }

  if (g_shaderCacheEntryCount > 0) {
    const ShaderCacheEntry& first = g_shaderCacheEntries[0];
    REXLOG_INFO("plume:   first entry hash={:016X} spirv={} bytes src={}", first.hash,
                first.spirvSize, first.source ? first.source : "?");
  }
}

}  // namespace rex::plume_renderer
