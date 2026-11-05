/**
 * @file        plume_renderer/plume_postfx.cpp
 * @brief       Anti-aliasing (FXAA, SMAA) and sharpening (CAS) of the finished frame.
 */
#include "plume_renderer/plume_postfx.h"
#include "plume_renderer/plume_draw.h"

#include "frame_clock_provider.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <plume_render_interface.h>

#include <rex/logging.h>

// SMAA's lookup tables, from its reference implementation (thirdparty/smaa):
// areaTexBytes, 160x560 of two bytes, and searchTexBytes, 64x16 of one.
#include <AreaTex.h>
#include <SearchTex.h>

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
    aa = uint32_t(AntiAliasingFromName(std::getenv("XERENGE_AA")));
    const char* sharpen_text = std::getenv("XERENGE_SHARPEN");
    sharpen = sharpen_text != nullptr ? ClampStrength(std::strtof(sharpen_text, nullptr)) : 0.0f;
  }
};

LiveSettings& Live() {
  static LiveSettings settings;
  return settings;
}

// What the shaders read (fxaa.frag, smaa.frag, cas.frag): in a push constant,
// as there is nothing else to the passes' state.
struct PassConstants {
  float texel[2];
  float sharpness;
  float pad;
};

// SMAA's edge target: one channel for an edge to the left of the pixel, one
// for an edge above it.
constexpr plume::RenderFormat kEdgesFormat = plume::RenderFormat::R8G8_UNORM;

}  // namespace

const char* AntiAliasingName(AntiAliasing mode) {
  switch (mode) {
    case AntiAliasing::kFxaa:
      return "fxaa";
    case AntiAliasing::kSmaa:
      return "smaa";
    default:
      return "off";
  }
}

AntiAliasing AntiAliasingFromName(const char* name) {
  for (AntiAliasing mode : {AntiAliasing::kFxaa, AntiAliasing::kSmaa}) {
    if (name != nullptr && std::strcmp(name, AntiAliasingName(mode)) == 0) {
      return mode;
    }
  }
  return AntiAliasing::kOff;
}

AntiAliasing GetAntiAliasing() {
  return AntiAliasing(Live().aa.load(std::memory_order_relaxed));
}

void SetAntiAliasing(AntiAliasing mode) {
  // Through the names: a value that is none of the modes becomes off.
  Live().aa.store(uint32_t(AntiAliasingFromName(AntiAliasingName(mode))),
                  std::memory_order_relaxed);
}

std::atomic<bool>& BloomOffFlag() {
  static std::atomic<bool> off{std::getenv("XERENGE_NO_BLOOM") != nullptr};
  return off;
}

bool BloomOff() {
  return BloomOffFlag().load(std::memory_order_relaxed);
}

float GetSharpening() {
  return Live().sharpen.load(std::memory_order_relaxed);
}

void SetSharpening(float strength) {
  Live().sharpen.store(ClampStrength(strength), std::memory_order_relaxed);
}

PlumePostFx::Settings PlumePostFx::Settings::Current() {
  // A choice made in the game's menu (Driver Details > Special Features), which
  // the game publishes beside its frame clock (game_timing.cpp): taken when it changes.
  if (const RexFrameClockProviderFn provider = RexFrameClockProvider()) {
    struct {
      uint64_t frame;
      uint32_t steps_this_frame, epoch;
      double state_steps, real_steps;
      uint32_t valid, vblanks_per_frame;
      int32_t menu_anti_aliasing;
      float menu_sharpening;
      int32_t menu_bloom;
    } clock{};
    clock.menu_anti_aliasing = -1;
    clock.menu_sharpening = -1.0f;
    clock.menu_bloom = -1;
    provider(&clock, sizeof(clock));
    static int32_t taken = -1;
    if (clock.menu_anti_aliasing >= 0 && clock.menu_anti_aliasing != taken) {
      taken = clock.menu_anti_aliasing;
      SetAntiAliasing(AntiAliasing(uint32_t(taken)));
    }
    static float taken_sharpening = -1.0f;
    if (clock.menu_sharpening >= 0.0f && clock.menu_sharpening != taken_sharpening) {
      taken_sharpening = clock.menu_sharpening;
      SetSharpening(taken_sharpening);
    }
    if (clock.menu_bloom >= 0) {
      BloomOffFlag().store(clock.menu_bloom == 0, std::memory_order_relaxed);
    }
  }
  Settings settings;
  settings.aa = GetAntiAliasing();
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
  // The passes exist as SPIR-V only so far: on D3D12 their pipelines are empty
  // shells, and drawn through them the frame came out black (the Xbox, aa =
  // fxaa). Off there until they are written for it.
  if (!PlumeUsesVulkan()) {
    pipelines_failed_ = true;
    REXLOG_WARN("plume: anti-aliasing and sharpening are Vulkan-only for now; off on D3D12");
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
    REXLOG_ERROR("plume: post-processing could not be set up; anti-aliasing and sharpening "
                 "stay off");
    return false;
  }

  fxaa_pipeline_ = MakePipeline(layout_.get(), fxaa_shader_.get(), kColorTargetFormat);
  cas_pipeline_ = MakePipeline(layout_.get(), cas_shader_.get(), kColorTargetFormat);
  if (!fxaa_pipeline_ || !cas_pipeline_) {
    REXLOG_ERROR("plume: post-processing pipelines failed; anti-aliasing and sharpening stay "
                 "off");
    fxaa_pipeline_.reset();
    cas_pipeline_.reset();
    return false;
  }
  pipelines_failed_ = false;
  return true;
}

bool PlumePostFx::EnsureSmaaPipelines() {
  if (smaa_blend_pipeline_) {
    return true;
  }
  if (smaa_failed_) {
    return false;
  }
  if (!PlumeUsesVulkan()) {
    smaa_failed_ = true;
    return false;
  }
  // Tried once, as above; without them the frame is not anti-aliased, and
  // sharpening goes on.
  smaa_failed_ = true;

  smaa_edges_shader_ = device_->createShader(kSmaaEdgesPsSpirv, kSmaaEdgesPsSpirv_count * 4,
                                             "main", plume::RenderShaderFormat::SPIRV);
  smaa_weights_shader_ = device_->createShader(kSmaaWeightsPsSpirv, kSmaaWeightsPsSpirv_count * 4,
                                               "main", plume::RenderShaderFormat::SPIRV);
  smaa_blend_shader_ = device_->createShader(kSmaaBlendPsSpirv, kSmaaBlendPsSpirv_count * 4,
                                             "main", plume::RenderShaderFormat::SPIRV);

  // Sets 0 to 2, each a texture the pass reads; the sampler is set 0's.
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, false);
  layout_builder.addPushConstant(0, 0, sizeof(PassConstants), plume::RenderShaderStageFlag::PIXEL);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.end();
  smaa_layout_ = layout_builder.create(device_);

  if (smaa_edges_shader_ && smaa_weights_shader_ && smaa_blend_shader_ && smaa_layout_) {
    smaa_edges_pipeline_ = MakePipeline(layout_.get(), smaa_edges_shader_.get(), kEdgesFormat);
    smaa_weights_pipeline_ =
        MakePipeline(smaa_layout_.get(), smaa_weights_shader_.get(), kColorTargetFormat);
    smaa_blend_pipeline_ =
        MakePipeline(smaa_layout_.get(), smaa_blend_shader_.get(), kColorTargetFormat);
  }
  if (!smaa_edges_pipeline_ || !smaa_weights_pipeline_ || !smaa_blend_pipeline_) {
    REXLOG_ERROR("plume: SMAA could not be set up; the frame is not anti-aliased");
    smaa_edges_pipeline_.reset();
    smaa_weights_pipeline_.reset();
    smaa_blend_pipeline_.reset();
    return false;
  }
  smaa_failed_ = false;
  return true;
}

std::unique_ptr<plume::RenderPipeline> PlumePostFx::MakePipeline(
    const plume::RenderPipelineLayout* layout, const plume::RenderShader* pixel_shader,
    plume::RenderFormat format) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout;
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = pixel_shader;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  return device_->createGraphicsPipeline(desc);
}

bool PlumePostFx::MakeStage(Stage* stage, plume::RenderTexture* scene, uint32_t width,
                            uint32_t height, bool as_target, plume::RenderFormat format) {
  const plume::RenderTexture* readable = scene;
  if (as_target) {
    stage->texture =
        device_->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, format));
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
  return MakeSet(stage, readable, format);
}

bool PlumePostFx::MakeSet(Stage* stage, const plume::RenderTexture* readable,
                          plume::RenderFormat format) {
  stage->view = readable->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  stage->set = set_builder_.create(device_);
  if (!stage->view || !stage->set) {
    return false;
  }
  stage->set->setTexture(0, readable, plume::RenderTextureLayout::SHADER_READ, stage->view.get());
  stage->set->setSampler(1, sampler_.get());
  return true;
}

bool PlumePostFx::MakeSmaaStages(plume::RenderCommandList* list, plume::RenderTexture* scene,
                                 uint32_t width, uint32_t height) {
  if (!edges_.texture && !MakeStage(&edges_, scene, width, height, true, kEdgesFormat)) {
    return false;
  }
  if (!weights_.texture && !MakeStage(&weights_, scene, width, height, true, kColorTargetFormat)) {
    return false;
  }
  if (search_.set) {
    return true;
  }
  // Both tables from one buffer, the search table after the area table.
  tables_staging_ =
      device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(AREATEX_SIZE + SEARCHTEX_SIZE));
  auto* bytes = tables_staging_ ? static_cast<uint8_t*>(tables_staging_->map()) : nullptr;
  if (!bytes) {
    return false;
  }
  std::memcpy(bytes, areaTexBytes, AREATEX_SIZE);
  std::memcpy(bytes + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
  tables_staging_->unmap();
  return MakeTable(list, &area_, AREATEX_WIDTH, AREATEX_HEIGHT, plume::RenderFormat::R8G8_UNORM,
                   0) &&
         MakeTable(list, &search_, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                   plume::RenderFormat::R8_UNORM, AREATEX_SIZE);
}

bool PlumePostFx::MakeTable(plume::RenderCommandList* list, Stage* stage, uint32_t width,
                            uint32_t height, plume::RenderFormat format, uint64_t offset) {
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.committed = true;
  stage->texture = device_->createTexture(desc);
  if (!stage->texture || !MakeSet(stage, stage->texture.get(), format)) {
    return false;
  }
  list->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(stage->texture.get(), plume::RenderTextureLayout::COPY_DEST));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(stage->texture.get(), 0, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(tables_staging_.get(), format, width,
                                                        height, 1, width, offset),
      0, 0, 0, nullptr);
  // Where it stays: a table is only read from here on.
  list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(stage->texture.get(), plume::RenderTextureLayout::SHADER_READ));
  return true;
}

void PlumePostFx::Pass(plume::RenderCommandList* list, plume::RenderTexture* source,
                       plume::RenderDescriptorSet* source_set, Stage* target,
                       plume::RenderPipeline* pipeline, uint32_t width, uint32_t height,
                       float sharpness, std::initializer_list<const Stage*> more, bool clear) {
  // The source, the target, and of `more` what an earlier pass wrote; the
  // tables are in the layout they are read in already.
  plume::RenderTextureBarrier barriers[4] = {
      plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(target->texture.get(), plume::RenderTextureLayout::COLOR_WRITE)};
  uint32_t barrier_count = 2;
  for (const Stage* stage : more) {
    if (stage->framebuffer && barrier_count < 4) {
      barriers[barrier_count++] = plume::RenderTextureBarrier(
          stage->texture.get(), plume::RenderTextureLayout::SHADER_READ);
    }
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, barriers, barrier_count);
  list->setFramebuffer(target->framebuffer.get());
  // Every pixel is written, or the target cleared: what it held is not loaded.
  list->setAttachmentAccess(1u, 0u);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  if (clear) {
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  list->setGraphicsPipelineLayout(more.size() != 0 ? smaa_layout_.get() : layout_.get());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(source_set, 0);
  uint32_t set_index = 1;
  for (const Stage* stage : more) {
    list->setGraphicsDescriptorSet(stage->set.get(), set_index++);
  }
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
  const bool fxaa = settings.aa == AntiAliasing::kFxaa;
  const bool smaa = settings.aa == AntiAliasing::kSmaa && EnsureSmaaPipelines();
  const bool sharpen = settings.sharpen > 0.0f;
  if (!fxaa && !smaa && !sharpen) {
    return scene;
  }
  const bool both = (fxaa || smaa) && sharpen;
  const bool ready =
      (scene_.set || MakeStage(&scene_, scene, width, height, false, kColorTargetFormat)) &&
      (stage_a_.texture || MakeStage(&stage_a_, scene, width, height, true, kColorTargetFormat)) &&
      (!both || stage_b_.texture ||
       MakeStage(&stage_b_, scene, width, height, true, kColorTargetFormat)) &&
      (!smaa || MakeSmaaStages(list, scene, width, height));
  if (!ready) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("plume: post-processing targets could not be made at {}x{}; anti-aliasing "
                   "and sharpening stay off",
                   width, height);
    }
    ReleaseTargets();
    return scene;
  }
  if (!announced_) {
    announced_ = true;
    REXLOG_INFO("plume: post-processing at {}x{} (anti-aliasing {}, sharpening {:.2f})", width,
                height, AntiAliasingName(fxaa || smaa ? settings.aa : AntiAliasing::kOff),
                settings.sharpen);
  }

  Stage* result = &stage_a_;
  if (fxaa) {
    Pass(list, scene, scene_.set.get(), &stage_a_, fxaa_pipeline_.get(), width, height, 0.0f);
  } else if (smaa) {
    // The edge pass leaves out the pixels with no edge: its target is cleared.
    Pass(list, scene, scene_.set.get(), &edges_, smaa_edges_pipeline_.get(), width, height, 0.0f,
         {}, true);
    Pass(list, edges_.texture.get(), edges_.set.get(), &weights_, smaa_weights_pipeline_.get(),
         width, height, 0.0f, {&area_, &search_});
    Pass(list, scene, scene_.set.get(), &stage_a_, smaa_blend_pipeline_.get(), width, height,
         0.0f, {&weights_});
  }
  if (sharpen) {
    if (fxaa || smaa) {
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
  for (Stage* stage : {&scene_, &stage_a_, &stage_b_, &edges_, &weights_, &area_, &search_}) {
    // The set before what it points into.
    stage->set.reset();
    stage->framebuffer.reset();
    stage->view.reset();
    stage->texture.reset();
  }
  tables_staging_.reset();
  if (announced_) {
    REXLOG_INFO("plume: post-processing targets released");
  }
  announced_ = false;
}

void PlumePostFx::Shutdown() {
  ReleaseTargets();
  fxaa_pipeline_.reset();
  cas_pipeline_.reset();
  smaa_edges_pipeline_.reset();
  smaa_weights_pipeline_.reset();
  smaa_blend_pipeline_.reset();
  smaa_layout_.reset();
  layout_.reset();
  sampler_.reset();
  vertex_shader_.reset();
  fxaa_shader_.reset();
  cas_shader_.reset();
  smaa_edges_shader_.reset();
  smaa_weights_shader_.reset();
  smaa_blend_shader_.reset();
  pipelines_failed_ = false;
  smaa_failed_ = false;
  device_ = nullptr;
}

}  // namespace rex::plume_renderer
