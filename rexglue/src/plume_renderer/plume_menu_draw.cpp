/**
 * @file        plume_renderer/plume_menu_draw.cpp
 * @brief       Dear ImGui's draw lists onto a plume framebuffer (the in-game menu).
 */
#include "plume_renderer/plume_menu_draw.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include <imgui.h>
#include <plume_vulkan.h>
#include <rex/logging.h>

#include "plume_renderer/plume_menu_spirv.h"
#include "plume_renderer/plume_swapchain.h"

namespace rex::plume_renderer {

namespace {

// ImGui's vertices as they come: position, texture coordinate, colour.
static_assert(sizeof(ImDrawVert) == 20 && sizeof(ImDrawIdx) == 2,
              "the menu's vertex layout is ImGui's default one");

// Frames a thing handed back is kept for: more than the frames that can be
// on the GPU at once, which is two with XERENGE_ASYNC_PRESENT.
constexpr uint32_t kRetireFrames = 4;

bool PipelineOk(const plume::RenderPipeline* pipeline) {
  const auto* vk_pipeline = static_cast<const plume::VulkanGraphicsPipeline*>(pipeline);
  return vk_pipeline && vk_pipeline->vk != VK_NULL_HANDLE;
}

bool BufferOk(const plume::RenderBuffer* buffer) {
  const auto* vk_buffer = static_cast<const plume::VulkanBuffer*>(buffer);
  return vk_buffer && vk_buffer->vk != VK_NULL_HANDLE;
}

bool TextureOk(const plume::RenderTexture* texture) {
  const auto* vk_texture = static_cast<const plume::VulkanTexture*>(texture);
  return vk_texture && vk_texture->vk != VK_NULL_HANDLE;
}

}  // namespace

PlumeMenuDraw::~PlumeMenuDraw() {
  Shutdown();
}

bool PlumeMenuDraw::Initialize(plume::RenderDevice* device) {
  Shutdown();
  device_ = device;
  if (!device_) {
    return false;
  }

  // One set: the atlas and the sampler it is read with; the transform goes in
  // as push constants.
  set_builder_.begin();
  set_builder_.addTexture(0);
  set_builder_.addSampler(1);
  set_builder_.end();
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);
  layout_builder.addPushConstant(0, 0, 4 * sizeof(float), plume::RenderShaderStageFlag::VERTEX);
  layout_builder.addDescriptorSet(set_builder_);
  layout_builder.end();
  layout_ = layout_builder.create(device_);

  vertex_shader_ = device_->createShader(kMenuVsSpirv, kMenuVsSpirv_count * 4, "main",
                                         plume::RenderShaderFormat::SPIRV);
  pixel_shader_ = device_->createShader(kMenuPsSpirv, kMenuPsSpirv_count * 4, "main",
                                        plume::RenderShaderFormat::SPIRV);

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.maxLOD = 0.0f;
  sampler_ = device_->createSampler(sampler_desc);

  if (!layout_ || !vertex_shader_ || !pixel_shader_ || !sampler_) {
    REXLOG_ERROR("plume: the menu's pipeline layout, shaders or sampler could not be made");
    Shutdown();
    return false;
  }

  input_slot_ = plume::RenderInputSlot(0, sizeof(ImDrawVert),
                                       plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  input_elements_[0] = plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT,
                                                 0, offsetof(ImDrawVert, pos));
  input_elements_[1] = plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT,
                                                 0, offsetof(ImDrawVert, uv));
  input_elements_[2] = plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM,
                                                 0, offsetof(ImDrawVert, col));
  return true;
}

void PlumeMenuDraw::Shutdown() {
  retired_.clear();
  pipelines_.clear();
  for (FrameBuffers& frame : buffers_) {
    frame = FrameBuffers{};
  }
  sampler_.reset();
  pixel_shader_.reset();
  vertex_shader_.reset();
  layout_.reset();
  device_ = nullptr;
}

template <typename T>
void PlumeMenuDraw::Retire(std::unique_ptr<T> object) {
  if (object) {
    retired_.emplace_back(frame_, std::shared_ptr<void>(std::move(object)));
  }
}

plume::RenderPipeline* PlumeMenuDraw::PipelineFor(const Target& target) {
  const uint32_t key = (target.color_count << 1) | (target.depth ? 1u : 0u);
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second.get();
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = pixel_shader_.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.frontFace = plume::RenderFrontFace::CLOCKWISE;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  // Declared whether or not the test is on: the framebuffer carries a depth
  // attachment or it does not, and the pipeline has to agree with it.
  desc.depthTargetFormat = target.depth ? kPlumeDepthFormat : plume::RenderFormat::UNKNOWN;
  desc.renderTargetCount = target.color_count;
  desc.renderTargetFormat[0] = kColorTargetFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  for (uint32_t i = 1; i < target.color_count; ++i) {
    // The frame's second target (motion vectors) is declared and left alone.
    desc.renderTargetFormat[i] = kColorTargetFormat;
    desc.renderTargetBlend[i] = plume::RenderBlendDesc::Copy();
    desc.renderTargetBlend[i].renderTargetWriteMask = 0;
  }
  desc.inputSlots = &input_slot_;
  desc.inputSlotsCount = 1;
  desc.inputElements = input_elements_;
  desc.inputElementsCount = 3;

  auto pipeline = device_->createGraphicsPipeline(desc);
  if (!pipeline || !PipelineOk(pipeline.get())) {
    REXLOG_ERROR("plume: the menu's pipeline could not be made ({} colour targets, depth {})",
                 target.color_count, target.depth);
    return nullptr;
  }
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

void PlumeMenuDraw::UpdateTexture(plume::RenderCommandList* list, ImTextureData* texture) {
  if (texture->Status == ImTextureStatus_WantDestroy) {
    if (texture->UnusedFrames > 0) {
      if (auto* gpu = static_cast<GpuTexture*>(texture->BackendUserData)) {
        Retire(std::unique_ptr<GpuTexture>(gpu));
      }
      texture->BackendUserData = nullptr;
      texture->SetTexID(ImTextureID_Invalid);
      texture->SetStatus(ImTextureStatus_Destroyed);
    }
    return;
  }
  if (texture->Status != ImTextureStatus_WantCreate &&
      texture->Status != ImTextureStatus_WantUpdates) {
    return;
  }
  if (texture->Format != ImTextureFormat_RGBA32) {
    REXLOG_ERROR("plume: the menu's atlas is not RGBA32");
    return;
  }
  const uint32_t width = uint32_t(texture->Width);
  const uint32_t height = uint32_t(texture->Height);
  auto* gpu = static_cast<GpuTexture*>(texture->BackendUserData);
  if (texture->Status == ImTextureStatus_WantCreate || !gpu) {
    auto made = std::make_unique<GpuTexture>();
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM);
    desc.committed = true;
    made->texture = device_->createTexture(desc);
    if (!made->texture || !TextureOk(made->texture.get())) {
      REXLOG_ERROR("plume: the menu's atlas texture could not be made");
      return;
    }
    made->view = made->texture->createTextureView(
        plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    made->set = set_builder_.create(device_);
    if (!made->view || !made->set) {
      return;
    }
    made->set->setTexture(0, made->texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          made->view.get());
    made->set->setSampler(1, sampler_.get());
    gpu = made.release();
    texture->BackendUserData = gpu;
    texture->SetTexID(ImTextureID(reinterpret_cast<uintptr_t>(gpu)));
  }

  // The whole atlas, however little of it changed: it is a few hundred
  // kilobytes, uploaded when a glyph is first drawn.
  auto staging = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(texture->GetSizeInBytes()));
  void* mapped = staging ? staging->map() : nullptr;
  if (!mapped) {
    return;
  }
  std::memcpy(mapped, texture->GetPixels(), size_t(texture->GetSizeInBytes()));
  staging->unmap();
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(gpu->texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(gpu->texture.get(), 0, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(
          staging.get(), plume::RenderFormat::R8G8B8A8_UNORM, width, height, 1, width, 0),
      0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(gpu->texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  Retire(std::move(staging));
  texture->SetStatus(ImTextureStatus_OK);
}

bool PlumeMenuDraw::EnsureBuffers(uint32_t slot, uint64_t vertex_bytes, uint64_t index_bytes) {
  FrameBuffers& frame = buffers_[slot];
  if (!frame.vertices || frame.vertex_bytes < vertex_bytes) {
    // Room to grow into: the menu's size changes with the page.
    const uint64_t bytes = std::max<uint64_t>(vertex_bytes * 2, 64 * 1024);
    Retire(std::move(frame.vertices));
    frame.vertices = device_->createBuffer(
        plume::RenderBufferDesc::VertexBuffer(bytes, plume::RenderHeapType::UPLOAD));
    frame.vertex_bytes = bytes;
    if (!frame.vertices || !BufferOk(frame.vertices.get())) {
      frame.vertices.reset();
      return false;
    }
  }
  if (!frame.indices || frame.index_bytes < index_bytes) {
    const uint64_t bytes = std::max<uint64_t>(index_bytes * 2, 32 * 1024);
    Retire(std::move(frame.indices));
    frame.indices = device_->createBuffer(
        plume::RenderBufferDesc::IndexBuffer(bytes, plume::RenderHeapType::UPLOAD));
    frame.index_bytes = bytes;
    if (!frame.indices || !BufferOk(frame.indices.get())) {
      frame.indices.reset();
      return false;
    }
  }
  return true;
}

void PlumeMenuDraw::Draw(plume::RenderCommandList* list, ImDrawData* data, const Target& target) {
  if (!device_ || !data || !data->Valid || target.width == 0 || target.height == 0) {
    return;
  }
  ++frame_;
  while (!retired_.empty() && retired_.front().first + kRetireFrames < frame_) {
    retired_.pop_front();
  }

  list->setFramebuffer(nullptr);
  if (data->Textures) {
    for (ImTextureData* texture : *data->Textures) {
      UpdateTexture(list, texture);
    }
  }
  list->setFramebuffer(target.framebuffer);
  if (data->CmdListsCount == 0 || data->TotalVtxCount == 0) {
    return;
  }

  plume::RenderPipeline* pipeline = PipelineFor(target);
  const uint32_t slot = frame_ & 1;
  if (!pipeline ||
      !EnsureBuffers(slot, uint64_t(data->TotalVtxCount) * sizeof(ImDrawVert),
                     uint64_t(data->TotalIdxCount) * sizeof(ImDrawIdx))) {
    return;
  }
  FrameBuffers& frame = buffers_[slot];
  auto* vertices = static_cast<ImDrawVert*>(frame.vertices->map());
  auto* indices = static_cast<ImDrawIdx*>(frame.indices->map());
  if (!vertices || !indices) {
    return;
  }
  for (const ImDrawList* commands : data->CmdLists) {
    std::memcpy(vertices, commands->VtxBuffer.Data, size_t(commands->VtxBuffer.Size) * sizeof(ImDrawVert));
    std::memcpy(indices, commands->IdxBuffer.Data, size_t(commands->IdxBuffer.Size) * sizeof(ImDrawIdx));
    vertices += commands->VtxBuffer.Size;
    indices += commands->IdxBuffer.Size;
  }
  frame.vertices->unmap();
  frame.indices->unmap();

  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(target.width), float(target.height)));
  list->setGraphicsPipelineLayout(layout_.get());
  list->setPipeline(pipeline);
  const plume::RenderVertexBufferView vertex_view(
      frame.vertices.get(), uint32_t(uint64_t(data->TotalVtxCount) * sizeof(ImDrawVert)));
  list->setVertexBuffers(0, &vertex_view, 1, &input_slot_);
  const plume::RenderIndexBufferView index_view(
      frame.indices.get(), uint32_t(uint64_t(data->TotalIdxCount) * sizeof(ImDrawIdx)),
      plume::RenderFormat::R16_UINT);
  list->setIndexBuffer(&index_view);

  // ImGui's pixels from the top left to the clip space, whose y points down
  // as well.
  const float transform[4] = {2.0f / data->DisplaySize.x, 2.0f / data->DisplaySize.y,
                              -1.0f - data->DisplayPos.x * 2.0f / data->DisplaySize.x,
                              -1.0f - data->DisplayPos.y * 2.0f / data->DisplaySize.y};
  list->setGraphicsPushConstants(0, transform);

  const GpuTexture* bound = nullptr;
  uint32_t vertex_offset = 0;
  uint32_t index_offset = 0;
  for (const ImDrawList* commands : data->CmdLists) {
    for (const ImDrawCmd& cmd : commands->CmdBuffer) {
      if (cmd.UserCallback) {
        continue;
      }
      const auto* gpu = reinterpret_cast<const GpuTexture*>(uintptr_t(cmd.GetTexID()));
      if (!gpu) {
        continue;
      }
      const float left = std::max(cmd.ClipRect.x - data->DisplayPos.x, 0.0f);
      const float top = std::max(cmd.ClipRect.y - data->DisplayPos.y, 0.0f);
      const float right = std::min(cmd.ClipRect.z - data->DisplayPos.x, float(target.width));
      const float bottom = std::min(cmd.ClipRect.w - data->DisplayPos.y, float(target.height));
      if (right <= left || bottom <= top) {
        continue;
      }
      list->setScissors(plume::RenderRect(int32_t(left), int32_t(top), int32_t(std::ceil(right)),
                                          int32_t(std::ceil(bottom))));
      if (gpu != bound) {
        list->setGraphicsDescriptorSet(gpu->set.get(), 0);
        bound = gpu;
      }
      list->drawIndexedInstanced(cmd.ElemCount, 1, index_offset + cmd.IdxOffset,
                                 int32_t(vertex_offset + cmd.VtxOffset), 0);
    }
    vertex_offset += uint32_t(commands->VtxBuffer.Size);
    index_offset += uint32_t(commands->IdxBuffer.Size);
  }
}

}  // namespace rex::plume_renderer
