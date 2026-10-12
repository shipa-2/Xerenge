/**
 * @file        plume_renderer/plume_postfx.cpp
 * @brief       Anti-aliasing (FXAA) and sharpening (CAS) of the finished frame.
 */
#include "plume_renderer/plume_postfx.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "plume_renderer/plume_postfx_spirv.h"
#include "plume_renderer/plume_swapchain.h"

namespace rex::plume_renderer {

namespace {

float ClampStrength(float strength) {
  // NaN fails both comparisons and would stay; it means off.
  return strength > 0.0f ? std::min(strength, 1.0f) : 0.0f;
}

struct LiveSettings {
  std::atomic<uint32_t> aa;
  std::atomic<float> sharpen;

  LiveSettings() {
    const char* aa_text = std::getenv("XERENGE_AA");
    aa = aa_text != nullptr && std::strcmp(aa_text, "fxaa") == 0
             ? uint32_t(AntiAliasing::kFxaa)
             : uint32_t(AntiAliasing::kOff);
    const char* sharpen_text = std::getenv("XERENGE_SHARPEN");
    sharpen = sharpen_text != nullptr ? ClampStrength(std::strtof(sharpen_text, nullptr)) : 0.0f;
  }
};

LiveSettings& Live() {
  static LiveSettings settings;
  return settings;
}

// What the shaders read (fxaa.frag, cas.frag): in a push constant, as there
// is nothing else to the passes' state.
struct PassConstants {
  float texel[2];
  float sharpness;
  float pad;
};

}  // namespace

AntiAliasing GetAntiAliasing() {
  return AntiAliasing(Live().aa.load(std::memory_order_relaxed));
}

void SetAntiAliasing(AntiAliasing mode) {
  Live().aa.store(uint32_t(mode == AntiAliasing::kFxaa ? AntiAliasing::kFxaa : AntiAliasing::kOff),
                  std::memory_order_relaxed);
}

float GetSharpening() {
  return Live().sharpen.load(std::memory_order_relaxed);
}

void SetSharpening(float strength) {
  Live().sharpen.store(ClampStrength(strength), std::memory_order_relaxed);
}

PlumePostFx::Settings PlumePostFx::Settings::Current() {
  Settings settings;
  settings.fxaa = GetAntiAliasing() == AntiAliasing::kFxaa;
  settings.sharpen = GetSharpening();
  return settings;
}

PlumePostFx::PlumePostFx() = default;

PlumePostFx::~PlumePostFx() {
  Shutdown();
}

bool PlumePostFx::EnsurePipelines() {
  if (fxaa_pipeline_ && cas_pipeline_) {
    return true;
  }
  if (pipelines_failed_ || !device_) {
    return false;
  }
  // Tried once: a driver that cannot make them would otherwise be asked every
  // frame, and the frame goes on without.
  pipelines_failed_ = true;

  vertex_shader_ = device_->createShader(kPostVsSpirv, kPostVsSpirv_count * 4, "main",
                                         plume::RenderShaderFormat::SPIRV);
  fxaa_shader_ = device_->createShader(kFxaaPsSpirv, kFxaaPsSpirv_count * 4, "main",
                                       plume::RenderShaderFormat::SPIRV);
  cas_shader_ = device_->createShader(kCasPsSpirv, kCasPsSpirv_count * 4, "main",
                                      plume::RenderShaderFormat::SPIRV);

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  sampler_ = device_->createSampler(sampler_desc);

  // Set 0: the texture a pass reads and the sampler it reads it with.
  set_builder_.begin();
  set_builder_.addTexture(0);
  set_builder_.addSampler(1);
  set_builder_.end();
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, false);
  layout_builder.addPushConstant(0, 0, sizeof(PassConstants), plume::RenderShaderStageFlag::PIXEL);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.end();
  layout_ = layout_builder.create(device_);

  if (!vertex_shader_ || !fxaa_shader_ || !cas_shader_ || !sampler_ || !layout_) {
    REXLOG_ERROR("plume: post-processing could not be set up; FXAA and sharpening stay off");
    return false;
  }

  auto make = [&](const plume::RenderShader* pixel_shader) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = vertex_shader_.get();
    desc.pixelShader = pixel_shader;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kColorTargetFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    return device_->createGraphicsPipeline(desc);
  };
  fxaa_pipeline_ = make(fxaa_shader_.get());
  cas_pipeline_ = make(cas_shader_.get());
  if (!fxaa_pipeline_ || !cas_pipeline_) {
    REXLOG_ERROR("plume: post-processing pipelines failed; FXAA and sharpening stay off");
    fxaa_pipeline_.reset();
    cas_pipeline_.reset();
    return false;
  }
  pipelines_failed_ = false;
  return true;
}

bool PlumePostFx::MakeStage(Stage* stage, plume::RenderTexture* scene, uint32_t width,
                            uint32_t height, bool as_target) {
  const plume::RenderTexture* readable = scene;
  if (as_target) {
    stage->texture = device_->createTexture(
        plume::RenderTextureDesc::ColorTarget(width, height, kColorTargetFormat));
    if (!stage->texture) {
      return false;
    }
    const plume::RenderTexture* attachment = stage->texture.get();
    plume::RenderFramebufferDesc framebuffer_desc;
    framebuffer_desc.colorAttachments = &attachment;
    framebuffer_desc.colorAttachmentsCount = 1;
    stage->framebuffer = device_->createFramebuffer(framebuffer_desc);
    if (!stage->framebuffer) {
      return false;
    }
    readable = stage->texture.get();
  }
  stage->view = readable->createTextureView(plume::RenderTextureViewDesc::Texture2D(kColorTargetFormat));
  stage->set = set_builder_.create(device_);
  if (!stage->view || !stage->set) {
    return false;
  }
  stage->set->setTexture(0, readable, plume::RenderTextureLayout::SHADER_READ, stage->view.get());
  stage->set->setSampler(1, sampler_.get());
  return true;
}

void PlumePostFx::Pass(plume::RenderCommandList* list, plume::RenderTexture* source,
                       plume::RenderDescriptorSet* source_set, Stage* target,
                       plume::RenderPipeline* pipeline, uint32_t width, uint32_t height,
                       float sharpness) {
  const plume::RenderTextureBarrier barriers[2] = {
      plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(target->texture.get(), plume::RenderTextureLayout::COLOR_WRITE)};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, barriers, 2);
  list->setFramebuffer(target->framebuffer.get());
  // Every pixel is written: what the target held is not loaded.
  list->setAttachmentAccess(1u, 0u);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  list->setGraphicsPipelineLayout(layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(source_set, 0);
  const PassConstants constants = {{1.0f / float(width), 1.0f / float(height)}, sharpness, 0.0f};
  list->setGraphicsPushConstants(0, &constants);
  list->drawInstanced(3, 1, 0, 0);
  // Closing the pass is what lets the next one (or the copy) read the target.
  list->setFramebuffer(nullptr);
}

plume::RenderTexture* PlumePostFx::Record(plume::RenderCommandList* list,
                                          plume::RenderTexture* scene, uint32_t width,
                                          uint32_t height, const Settings& settings) {
  if (!settings.Any() || !scene || width == 0 || height == 0 || !EnsurePipelines()) {
    return scene;
  }
  const bool both = settings.fxaa && settings.sharpen > 0.0f;
  const bool ready =
      (scene_.set || MakeStage(&scene_, scene, width, height, false)) &&
      (stage_a_.texture || MakeStage(&stage_a_, scene, width, height, true)) &&
      (!both || stage_b_.texture || MakeStage(&stage_b_, scene, width, height, true));
  if (!ready) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("plume: post-processing targets could not be made at {}x{}; FXAA and "
                   "sharpening stay off",
                   width, height);
    }
    ReleaseTargets();
    return scene;
  }
  if (!announced_) {
    announced_ = true;
    REXLOG_INFO("plume: post-processing at {}x{} (FXAA {}, sharpening {:.2f})", width, height,
                settings.fxaa ? "on" : "off", settings.sharpen);
  }

  Stage* result = &stage_a_;
  if (settings.fxaa) {
    Pass(list, scene, scene_.set.get(), &stage_a_, fxaa_pipeline_.get(), width, height, 0.0f);
  }
  if (settings.sharpen > 0.0f) {
    if (settings.fxaa) {
      Pass(list, stage_a_.texture.get(), stage_a_.set.get(), &stage_b_, cas_pipeline_.get(), width,
           height, settings.sharpen);
      result = &stage_b_;
    } else {
      Pass(list, scene, scene_.set.get(), &stage_a_, cas_pipeline_.get(), width, height,
           settings.sharpen);
    }
  }
  return result->texture.get();
}

void PlumePostFx::ReleaseTargets() {
  for (Stage* stage : {&scene_, &stage_a_, &stage_b_}) {
    // The set before what it points into.
    stage->set.reset();
    stage->framebuffer.reset();
    stage->view.reset();
    stage->texture.reset();
  }
  if (announced_) {
    REXLOG_INFO("plume: post-processing targets released");
  }
  announced_ = false;
}

void PlumePostFx::Shutdown() {
  ReleaseTargets();
  fxaa_pipeline_.reset();
  cas_pipeline_.reset();
  layout_.reset();
  sampler_.reset();
  vertex_shader_.reset();
  fxaa_shader_.reset();
  cas_shader_.reset();
  pipelines_failed_ = false;
  device_ = nullptr;
}

}  // namespace rex::plume_renderer
