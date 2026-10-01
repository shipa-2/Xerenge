/**
 * @file        plume_renderer/plume_shader_cache.cpp
 * @brief       XenosRecomp offline SPIR-V cache → plume::RenderShader (phase 2).
 */
#include "plume_renderer/plume_shader_cache.h"

#include <algorithm>
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

plume::RenderShader* PlumeShaderCache::GetOrCreateShader(uint64_t hash) {
  std::lock_guard lock(mutex_);
  if (!device_) {
    return nullptr;
  }

  if (auto it = shaders_.find(hash); it != shaders_.end()) {
    return it->second.get();
  }

  const ShaderCacheEntry* entry = Find(hash);
  if (!entry) {
    return nullptr;
  }

  size_t word_count = 0;
  const uint32_t* words = GetSpirvWords(*entry, word_count);
  if (!words || word_count == 0) {
    return nullptr;
  }

  // No SV_ClipDistance for a GPU that cannot take it (see StripClipDistance).
  std::vector<uint32_t> stripped;
  if (!device_->getCapabilities().clipDistance && StripClipDistance(words, word_count, &stripped)) {
    words = stripped.data();
    word_count = stripped.size();
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
