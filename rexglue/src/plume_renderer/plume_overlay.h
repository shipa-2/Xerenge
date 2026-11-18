/**
 * @file        plume_renderer/plume_overlay.h
 * @brief       A picture the game hands over (an achievement's notice), laid over the shown frame.
 */
#pragma once

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <plume_render_interface_types.h>

namespace rex::plume_renderer {

// The game draws the picture itself, with the processor, and publishes it with
// its frame clock (game_timing.cpp, achievement_toast.cpp): straight RGBA, its
// size at a 1080-line window, a generation that changes with every new picture
// and an opacity for fading it. The swapchain lays it over the window's image
// after the frame is copied there, at the top, in the middle, scaled to the
// window's height.
class PlumeOverlay {
 public:
  explicit PlumeOverlay(plume::RenderDevice* device) : device_(device) {}

  // Outside a render pass: takes the game's pictures and, when one is new,
  // uploads it. False when there is nothing to show.
  bool Prepare(plume::RenderCommandList* list, plume::RenderFormat format);
  // Into the framebuffer bound, the window's image of window_width x
  // window_height (of the format given to Prepare), after Prepare said there
  // was something to show.
  void Draw(plume::RenderCommandList* list, uint32_t window_width, uint32_t window_height);

 private:
  bool EnsurePipeline(plume::RenderFormat format);

  // One picture's GPU state: the texture of the current generation, the
  // previous kept until the next (frames still in flight read it), and how
  // opaque it is now.
  struct Picture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderDescriptorSet> set;
    std::unique_ptr<plume::RenderBuffer> staging;
    std::unique_ptr<plume::RenderTexture> old_texture;
    std::unique_ptr<plume::RenderTextureView> old_view;
    std::unique_ptr<plume::RenderDescriptorSet> old_set;
    std::unique_ptr<plume::RenderBuffer> old_staging;
    uint32_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float opacity = 0.0f;
  };
  // The game's picture into `picture`; false when there is nothing to show.
  bool PreparePicture(plume::RenderCommandList* list, plume::RenderFormat format,
                      const uint8_t* pixels, uint32_t width, uint32_t height,
                      uint32_t generation, float opacity, Picture& picture);

  plume::RenderDevice* device_;
  bool failed_ = false;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> pixel_shader_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;

  Picture toast_;   // the achievements' notice, anchored at the top
  Picture prompt_;  // the Song Manager's X prompt, anchored at the bottom
};

}  // namespace rex::plume_renderer
