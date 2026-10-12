/**
 * @file        plume_renderer/plume_menu_draw.h
 * @brief       Dear ImGui's draw lists onto a plume framebuffer (the in-game menu).
 */
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <utility>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

struct ImDrawData;
struct ImTextureData;

namespace rex::plume_renderer {

// The renderer half of the menu: Dear ImGui builds the geometry, this puts it
// on a framebuffer. It owns the pipelines, the font atlas texture and the
// per-frame vertex and index buffers, and nothing else of the renderer's.
class PlumeMenuDraw {
 public:
  PlumeMenuDraw() = default;
  ~PlumeMenuDraw();

  PlumeMenuDraw(const PlumeMenuDraw&) = delete;
  PlumeMenuDraw& operator=(const PlumeMenuDraw&) = delete;

  bool Initialize(plume::RenderDevice* device);
  void Shutdown();

  // The framebuffer the menu goes on. A pipeline has to agree with it on the
  // number of colour attachments and on whether there is a depth one.
  struct Target {
    const plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t color_count = 1;
    bool depth = false;
  };

  // Records the menu. `list` must have no framebuffer bound (the atlas is
  // uploaded first, and a copy cannot happen inside a render pass); the
  // target is bound when this returns.
  void Draw(plume::RenderCommandList* list, ImDrawData* data, const Target& target);

 private:
  struct GpuTexture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };

  plume::RenderPipeline* PipelineFor(const Target& target);
  void UpdateTexture(plume::RenderCommandList* list, ImTextureData* texture);
  bool EnsureBuffers(uint32_t slot, uint64_t vertex_bytes, uint64_t index_bytes);
  // Held until the frames that may still read it are over.
  template <typename T>
  void Retire(std::unique_ptr<T> object);

  plume::RenderDevice* device_ = nullptr;
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> pixel_shader_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  plume::RenderInputSlot input_slot_;
  plume::RenderInputElement input_elements_[3];
  // By colour attachments and depth: (count << 1) | depth.
  std::map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;

  // Two sets of buffers, taking turns as the frames do.
  struct FrameBuffers {
    std::unique_ptr<plume::RenderBuffer> vertices;
    std::unique_ptr<plume::RenderBuffer> indices;
    uint64_t vertex_bytes = 0;
    uint64_t index_bytes = 0;
  };
  FrameBuffers buffers_[2];
  uint32_t frame_ = 0;

  std::deque<std::pair<uint32_t, std::shared_ptr<void>>> retired_;
};

}  // namespace rex::plume_renderer
