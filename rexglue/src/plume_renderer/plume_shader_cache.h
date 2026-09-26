/**
 * @file        plume_renderer/plume_shader_cache.h
 * @brief       XenosRecomp offline SPIR-V cache → plume::RenderShader (phase 2).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct ShaderCacheEntry;
struct ShaderMicrocodeEntry;

namespace plume {
class RenderDevice;
class RenderShader;
}  // namespace plume

namespace rex::plume_renderer {

class PlumeShaderCache {
 public:
  static PlumeShaderCache& Instance();

  void SetDevice(plume::RenderDevice* device);
  bool IsAvailable() const;
  size_t EntryCount() const;

  const ShaderCacheEntry* Find(uint64_t hash) const;
  const ShaderMicrocodeEntry* FindByMicrocode(uint64_t microcode_hash) const;
  // Every microcode length the cache knows for a stage (0 vertex, 1 pixel),
  // ascending. Lets a shader be recognised from its microcode pointer alone:
  // each candidate is a length some real shader has, so hashing that many
  // bytes never reads past a shader into guesswork.
  const std::vector<uint32_t>& MicrocodeSizes(uint32_t stage);
  plume::RenderShader* GetOrCreateShader(uint64_t hash);
  uint64_t HashShaderContainer(const void* shader_container) const;
  void LogSummary() const;
  void WarmAll();

 private:
  PlumeShaderCache() = default;

  bool EnsureDecompressed();
  bool EnsureExtraDecompressed();
  bool EnsureBootstrapDecompressed();
  const uint32_t* GetSpirvWords(const ShaderCacheEntry& entry, size_t& word_count);

  plume::RenderDevice* device_ = nullptr;
  std::mutex mutex_;
  std::vector<uint8_t> decompressed_spirv_;
  std::vector<uint32_t> microcode_sizes_[2];
  bool microcode_sizes_built_ = false;
  std::vector<uint8_t> extra_decompressed_spirv_;
  std::vector<uint8_t> bootstrap_decompressed_spirv_;
  bool decompressed_valid_ = false;
  bool extra_decompressed_valid_ = false;
  bool bootstrap_decompressed_valid_ = false;
  std::unordered_map<uint64_t, std::vector<uint32_t>> spirv_modules_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders_;
};

}  // namespace rex::plume_renderer
