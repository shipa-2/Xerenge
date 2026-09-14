/**
 * @file        plume_renderer/plume_draw.h
 * @brief       Guest DRAW_INDX_2 → plume graphics pipeline (present thread).
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_types.h>

namespace rex::memory {
class Memory;
}

namespace rex::plume_renderer {

struct VfetchAttr {
  uint32_t fetch_const = 0;
  uint32_t stride_dwords = 0;
  int32_t offset_dwords = 0;
  uint32_t format = 0;
  int32_t exp_adjust = 0;
  uint32_t location = 0;
  bool is_signed = false;
  bool is_normalized = true;
};

struct GuestDrawSnapshot {
  uint64_t vs_hash = 0;
  uint64_t ps_hash = 0;
  uint32_t prim_type = 0;
  uint32_t source_select = 0;
  uint32_t num_indices = 0;
  uint32_t vte_cntl = 0;
  float vport_xscale = 0.0f;
  float vport_xoffset = 0.0f;
  float vport_yscale = 0.0f;
  float vport_yoffset = 0.0f;
  float vport_zscale = 0.0f;
  float vport_zoffset = 0.0f;
  std::array<uint32_t, 1024> vs_constants{};
  std::array<uint32_t, 1024> ps_constants{};
  std::array<uint32_t, 192> fetch_constants{};
  uint32_t vs_bool = 0;
  uint32_t ps_bool = 0;
  bool valid = false;
};

class PlumeDrawContext {
 public:
  PlumeDrawContext() = default;
  ~PlumeDrawContext();

  PlumeDrawContext(const PlumeDrawContext&) = delete;
  PlumeDrawContext& operator=(const PlumeDrawContext&) = delete;

  bool Initialize(plume::RenderDevice* device);
  void Shutdown();
  bool IsReady() const { return ready_; }

  void RegisterVsUcode(uint64_t shader_hash, const uint8_t* bytes, uint32_t byte_size);
  void EncodeDraws(plume::RenderCommandList* list, const std::vector<GuestDrawSnapshot>& draws,
                   memory::Memory* memory, uint32_t width, uint32_t height);

 private:
  struct PipelineKey {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t topology = 0;

    bool operator==(const PipelineKey& other) const {
      return vs == other.vs && ps == other.ps && topology == other.topology;
    }
  };
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const {
      return size_t(key.vs ^ (key.ps * 0x9E3779B97F4A7C15ull) ^ uint64_t(key.topology));
    }
  };

  plume::RenderPrimitiveTopology MapTopology(uint32_t prim_type) const;
  plume::RenderPipeline* GetOrCreatePipeline(uint64_t vs_hash, uint64_t ps_hash,
                                             plume::RenderPrimitiveTopology topology);
  uint32_t FillVertices(const GuestDrawSnapshot& snap, memory::Memory* memory,
                        uint32_t base_vertex, bool passthrough);
  void UploadNullTexture(plume::RenderCommandList* list);
  void BindGuestTextures(plume::RenderCommandList* list, const std::vector<GuestDrawSnapshot>& draws,
                         memory::Memory* memory);
  uint32_t BindlessForTexture(const GuestDrawSnapshot& snap) const;
  uint32_t BindlessForSlot(const GuestDrawSnapshot& snap, uint32_t slot) const;
  void FillSharedConstants(uint8_t* dst, const GuestDrawSnapshot& snap, uint32_t width,
                           uint32_t height) const;
  bool CreatePassthroughPipeline();
  plume::RenderPipeline* PassthroughFor(plume::RenderPrimitiveTopology topology);

  struct GuestHostTexture {
    uint64_t key = 0;
    uint32_t bindless = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    plume::RenderFormat format = plume::RenderFormat::R8G8B8A8_UNORM;
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderBuffer> staging;
    bool uploaded = false;
  };

  plume::RenderDevice* device_ = nullptr;
  bool ready_ = false;
  bool null_texture_uploaded_ = false;
  bool last_fill_passthrough_ = false;

  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout_;
  std::unique_ptr<plume::RenderDescriptorSet> texture_set_;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  std::unique_ptr<plume::RenderTexture> null_texture_;
  std::unique_ptr<plume::RenderTextureView> null_texture_view_;
  std::unique_ptr<plume::RenderBuffer> null_texture_staging_;
  std::unique_ptr<plume::RenderShader> passthrough_vs_;
  std::unique_ptr<plume::RenderShader> passthrough_ps_;
  std::unique_ptr<plume::RenderPipeline> passthrough_tri_;
  std::unique_ptr<plume::RenderPipeline> passthrough_point_;
  std::unique_ptr<plume::RenderPipeline> passthrough_fan_;
  std::unique_ptr<plume::RenderPipeline> passthrough_strip_;

  std::unique_ptr<plume::RenderBuffer> vs_constants_;
  std::unique_ptr<plume::RenderBuffer> ps_constants_;
  std::unique_ptr<plume::RenderBuffer> shared_constants_;
  std::unique_ptr<plume::RenderBuffer> dummy_vb_;
  void* vs_constants_mapped_ = nullptr;
  void* ps_constants_mapped_ = nullptr;
  void* shared_constants_mapped_ = nullptr;
  float* vb_mapped_ = nullptr;
  uint64_t vs_constants_addr_ = 0;
  uint64_t ps_constants_addr_ = 0;
  uint64_t shared_constants_addr_ = 0;

  std::array<plume::RenderInputSlot, 1> input_slots_{};
  std::array<plume::RenderInputElement, 32> input_elements_{};
  plume::RenderVertexBufferView vb_view_{};

  std::mutex vfetch_mutex_;
  std::unordered_map<uint64_t, std::vector<VfetchAttr>> vfetch_by_shader_;

  std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash>
      pipelines_;
  std::unordered_map<uint64_t, std::unique_ptr<GuestHostTexture>> guest_textures_;
  uint32_t next_bindless_ = 1;
};

}  // namespace rex::plume_renderer
