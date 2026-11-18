#include "plume_renderer/plume_overlay.h"

#include <algorithm>
#include <cstring>

#include <rex/logging.h>

#include "frame_clock_provider.h"
#include "plume_renderer/plume_overlay_spirv.h"

namespace rex::plume_renderer {
namespace {

constexpr plume::RenderFormat kPictureFormat = plume::RenderFormat::R8G8B8A8_UNORM;

// The game's frame clock as far as the overlay's fields (game_timing.cpp's
// FrameClock, in the same order).
struct Clock {
  uint64_t frame;
  uint32_t steps_this_frame, epoch;
  double state_steps, real_steps;
  uint32_t valid, vblanks_per_frame;
  int32_t menu_anti_aliasing;
  float menu_sharpening;
  int32_t menu_bloom;
  const uint8_t* overlay_pixels;
  uint32_t overlay_width, overlay_height, overlay_generation;
  float overlay_opacity;
  const uint8_t* overlay2_pixels;
  uint32_t overlay2_width, overlay2_height, overlay2_generation;
  float overlay2_opacity;
};

struct Constants {
  float rect[4];
  float opacity;
};

}  // namespace

bool PlumeOverlay::EnsurePipeline(plume::RenderFormat format) {
  if (pipeline_) {
    return true;
  }
  if (failed_ || !device_) {
    return false;
  }
  // Tried once: without it the game goes on without the notices.
  failed_ = true;
  vertex_shader_ = device_->createShader(kOverlayVsSpirv, kOverlayVsSpirv_count * 4, "main",
                                         plume::RenderShaderFormat::SPIRV);
  pixel_shader_ = device_->createShader(kOverlayPsSpirv, kOverlayPsSpirv_count * 4, "main",
                                        plume::RenderShaderFormat::SPIRV);
  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  sampler_ = device_->createSampler(sampler_desc);

  set_builder_.begin();
  set_builder_.addTexture(0);
  set_builder_.addSampler(1);
  set_builder_.end();
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, false);
  layout_builder.addPushConstant(0, 0, sizeof(Constants),
                                 plume::RenderShaderStageFlag::VERTEX |
                                     plume::RenderShaderStageFlag::PIXEL);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.end();
  layout_ = layout_builder.create(device_);
  if (!vertex_shader_ || !pixel_shader_ || !sampler_ || !layout_) {
    REXLOG_ERROR("plume: the overlay could not be set up; achievements are not shown");
    return false;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = pixel_shader_.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  pipeline_ = device_->createGraphicsPipeline(desc);
  if (!pipeline_) {
    REXLOG_ERROR("plume: the overlay's pipeline failed; achievements are not shown");
    return false;
  }
  failed_ = false;
  return true;
}

bool PlumeOverlay::PreparePicture(plume::RenderCommandList* list, plume::RenderFormat format,
                                  const uint8_t* pixels, uint32_t width, uint32_t height,
                                  uint32_t generation, float opacity, Picture& picture) {
  picture.opacity = std::clamp(opacity, 0.0f, 1.0f);
  if (!pixels || !width || !height || picture.opacity <= 0.0f) {
    return false;
  }
  if (!EnsurePipeline(format)) {
    return false;
  }
  if (generation == picture.generation && picture.texture) {
    return true;
  }
  // A new picture: into a texture of its own, through a buffer the processor
  // fills. The game keeps the pixels until two pictures later.
  const uint64_t bytes = uint64_t(width) * height * 4;
  picture.old_texture = std::move(picture.texture);
  picture.old_view = std::move(picture.view);
  picture.old_set = std::move(picture.set);
  picture.old_staging = std::move(picture.staging);
  picture.staging = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
  void* mapped = picture.staging ? picture.staging->map() : nullptr;
  if (!mapped) {
    return false;
  }
  std::memcpy(mapped, pixels, bytes);
  picture.staging->unmap();
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, kPictureFormat);
  desc.committed = true;
  picture.texture = device_->createTexture(desc);
  if (!picture.texture) {
    return false;
  }
  picture.view = picture.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kPictureFormat));
  picture.set = set_builder_.create(device_);
  if (!picture.view || !picture.set) {
    picture.texture.reset();
    return false;
  }
  picture.set->setTexture(0, picture.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          picture.view.get());
  picture.set->setSampler(1, sampler_.get());
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(picture.texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(picture.texture.get(), 0, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(picture.staging.get(), kPictureFormat,
                                                        width, height, 1, width, 0),
      0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(picture.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  picture.generation = generation;
  picture.width = width;
  picture.height = height;
  return true;
}

bool PlumeOverlay::Prepare(plume::RenderCommandList* list, plume::RenderFormat format) {
  const RexFrameClockProviderFn provider = RexFrameClockProvider();
  if (!provider || failed_) {
    return false;
  }
  Clock clock{};
  provider(&clock, sizeof(clock));
  const bool toast =
      PreparePicture(list, format, clock.overlay_pixels, clock.overlay_width, clock.overlay_height,
                     clock.overlay_generation, clock.overlay_opacity, toast_);
  const bool prompt =
      PreparePicture(list, format, clock.overlay2_pixels, clock.overlay2_width,
                     clock.overlay2_height, clock.overlay2_generation, clock.overlay2_opacity,
                     prompt_);
  return toast || prompt;
}

void PlumeOverlay::Draw(plume::RenderCommandList* list, uint32_t window_width,
                        uint32_t window_height) {
  if (!pipeline_) {
    return;
  }
  // Each picture's size at 1080 lines, scaled to the window; the toast 36
  // lines below the top, the prompt 36 above the bottom, both centred.
  const float scale = float(window_height) / 1080.0f;
  auto draw_picture = [&](const Picture& picture, float y) {
    if (!picture.texture) {
      return;
    }
    const float w = float(picture.width) * scale;
    const float h = float(picture.height) * scale;
    const float x = (float(window_width) - w) * 0.5f;
    Constants constants{};
    constants.rect[0] = x / float(window_width) * 2.0f - 1.0f;
    constants.rect[1] = y / float(window_height) * 2.0f - 1.0f;
    constants.rect[2] = (x + w) / float(window_width) * 2.0f - 1.0f;
    constants.rect[3] = (y + h) / float(window_height) * 2.0f - 1.0f;
    constants.opacity = picture.opacity;
    list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(window_width), float(window_height)));
    list->setScissors(plume::RenderRect(0, 0, int32_t(window_width), int32_t(window_height)));
    list->setGraphicsPipelineLayout(layout_.get());
    list->setPipeline(pipeline_.get());
    list->setGraphicsDescriptorSet(picture.set.get(), 0);
    list->setGraphicsPushConstants(0, &constants);
    list->drawInstanced(6, 1, 0, 0);
  };
  draw_picture(toast_, 36.0f * scale);
  const float prompt_h = float(prompt_.height) * scale;
  draw_picture(prompt_, float(window_height) - 36.0f * scale - prompt_h);
}

}  // namespace rex::plume_renderer
