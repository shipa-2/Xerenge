/**
 * @file        plume_renderer/plume_draw.cpp
 * @brief       Guest DRAW_INDX_2 → plume graphics pipeline (present thread).
 */
#include "plume_renderer/plume_draw.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

#include <plume_render_interface_builders.h>
#include <plume_vulkan.h>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>

#include "plume_renderer/plume_passthrough_spirv.h"
#include "plume_renderer/plume_shader_cache.h"

namespace rex::plume_renderer {

namespace {

constexpr uint32_t kBindlessTextureCount = 1024;
constexpr uint32_t kBindlessSamplerCount = 16;
constexpr uint32_t kPushConstantBytes = 24;
constexpr uint32_t kVsConstantBytes = 256 * 16;
constexpr uint32_t kPsConstantBytes = 256 * 16;
constexpr uint32_t kSharedConstantBytes = 512;
constexpr uint32_t kDummyVertexCount = 4096;
constexpr uint32_t kInputLocationCount = 32;
constexpr uint32_t kVertexStrideBytes = kInputLocationCount * 16;
constexpr uint32_t kCbSlots = 96;
constexpr uint32_t kCbAlign = 256;
constexpr uint32_t kVsSlotBytes = (kVsConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);
constexpr uint32_t kPsSlotBytes = (kPsConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);
constexpr uint32_t kSharedSlotBytes = (kSharedConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);

bool Float4Dead(const float* v) {
  return std::fabs(v[0]) < 1e-8f && std::fabs(v[1]) < 1e-8f && std::fabs(v[2]) < 1e-8f &&
         std::fabs(v[3]) < 1e-8f;
}

bool VulkanPipelineOk(const plume::RenderPipeline* pipeline) {
  const auto* vk_pipeline = static_cast<const plume::VulkanGraphicsPipeline*>(pipeline);
  return vk_pipeline && vk_pipeline->vk != VK_NULL_HANDLE;
}

bool VulkanBufferOk(const plume::RenderBuffer* buffer) {
  const auto* vk_buffer = static_cast<const plume::VulkanBuffer*>(buffer);
  return vk_buffer && vk_buffer->vk != VK_NULL_HANDLE;
}

uint32_t LoadBeU32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

std::vector<VfetchAttr> ParseVfetches(const uint8_t* bytes, uint32_t byte_size) {
  std::vector<VfetchAttr> attrs;
  if (!bytes || byte_size < 12) {
    return attrs;
  }

  using rex::graphics::ucode::ControlFlowInstruction;
  using rex::graphics::ucode::ControlFlowOpcode;
  using rex::graphics::ucode::FetchOpcode;
  using rex::graphics::ucode::UnpackControlFlowInstructions;
  using rex::graphics::ucode::VertexFetchInstruction;

  uint32_t instr_size = byte_size;
  uint32_t cf_off = 0;
  while (cf_off + 12 <= instr_size) {
    uint32_t words[3] = {LoadBeU32(bytes + cf_off), LoadBeU32(bytes + cf_off + 4),
                         LoadBeU32(bytes + cf_off + 8)};
    ControlFlowInstruction cf[2]{};
    UnpackControlFlowInstructions(words, cf);
    for (const auto& instr : cf) {
      uint32_t address = 0;
      switch (instr.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          address = instr.exec.address();
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          address = instr.cond_exec.address();
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          address = instr.cond_exec_pred.address();
          break;
        default:
          break;
      }
      if (address != 0) {
        instr_size = std::min(instr_size, address * 12);
      }
    }
    cf_off += 12;
  }

  uint32_t last_const = 0;
  uint32_t last_stride = 0;
  uint32_t next_texcoord = 13;
  uint32_t next_color = 17;
  cf_off = 0;
  while (cf_off + 12 <= instr_size) {
    uint32_t words[3] = {LoadBeU32(bytes + cf_off), LoadBeU32(bytes + cf_off + 4),
                         LoadBeU32(bytes + cf_off + 8)};
    ControlFlowInstruction cf[2]{};
    UnpackControlFlowInstructions(words, cf);
    for (const auto& instr : cf) {
      uint32_t address = 0;
      uint32_t count = 0;
      uint32_t sequence = 0;
      switch (instr.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          address = instr.exec.address();
          count = instr.exec.count();
          sequence = instr.exec.sequence();
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          address = instr.cond_exec.address();
          count = instr.cond_exec.count();
          sequence = instr.cond_exec.sequence();
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          address = instr.cond_exec_pred.address();
          count = instr.cond_exec_pred.count();
          sequence = instr.cond_exec_pred.sequence();
          break;
        default:
          break;
      }
      if (count == 0 || address * 12 + count * 12 > byte_size) {
        continue;
      }
      const uint8_t* code = bytes + address * 12;
      for (uint32_t i = 0; i < count; ++i) {
        if ((sequence & 1u) != 0) {
          const uint32_t raw[3] = {LoadBeU32(code + i * 12), LoadBeU32(code + i * 12 + 4),
                                   LoadBeU32(code + i * 12 + 8)};
          VertexFetchInstruction vfetch{};
          std::memcpy(&vfetch, raw, sizeof(raw));
          if (vfetch.opcode() == FetchOpcode::kVertexFetch) {
            if (!vfetch.is_mini_fetch()) {
              last_const = vfetch.fetch_constant_index();
              last_stride = vfetch.stride();
            }
            VfetchAttr attr;
            attr.fetch_const = last_const;
            attr.stride_dwords = last_stride;
            attr.offset_dwords = vfetch.offset();
            attr.format = uint32_t(vfetch.data_format());
            attr.exp_adjust = vfetch.exp_adjust();
            attr.is_signed = vfetch.is_signed();
            attr.is_normalized = vfetch.is_normalized();
            // XenosRecomp USAGE_LOCATIONS: POSITION0=0, TEXCOORD0=13, COLOR0=17.
            using rex::graphics::xenos::VertexFormat;
            const auto fmt = static_cast<VertexFormat>(attr.format);
            const bool colorish =
                fmt == VertexFormat::k_8_8_8_8 || fmt == VertexFormat::k_2_10_10_10 ||
                fmt == VertexFormat::k_16_16_16_16 || fmt == VertexFormat::k_16_16_16_16_FLOAT ||
                fmt == VertexFormat::k_32_32_32_32 || fmt == VertexFormat::k_32_32_32_32_FLOAT;
            if (attrs.empty()) {
              attr.location = 0;
            } else if (colorish) {
              attr.location = next_color++;
            } else {
              attr.location = next_texcoord++;
            }
            if (attr.location < kInputLocationCount) {
              attrs.push_back(attr);
            }
          }
        }
        sequence >>= 2;
      }
    }
    cf_off += 12;
  }
  return attrs;
}

constexpr uint32_t kFloatsPerVert = kVertexStrideBytes / 4;

float Dist2XY(const float* a, const float* b) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  return dx * dx + dy * dy;
}

// Xenos rectangle lists are 3 verts; the longest edge is the diagonal.
// Mirror the right-angle vertex across that edge to get the fourth corner.
uint32_t ExpandRectangle(float* vb, uint32_t base_vertex, uint32_t vertex_count) {
  if (vertex_count < 3 || base_vertex + 6 > kDummyVertexCount) {
    return vertex_count;
  }
  auto vert = [&](uint32_t i) { return vb + (base_vertex + i) * kFloatsPerVert; };
  std::vector<float> v0(vert(0), vert(0) + kFloatsPerVert);
  std::vector<float> v1(vert(1), vert(1) + kFloatsPerVert);
  std::vector<float> v2(vert(2), vert(2) + kFloatsPerVert);
  std::vector<float> v3(kFloatsPerVert);
  const float d01 = Dist2XY(v0.data(), v1.data());
  const float d02 = Dist2XY(v0.data(), v2.data());
  const float d12 = Dist2XY(v1.data(), v2.data());
  if (d01 >= d02 && d01 >= d12) {
    for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
      v3[c] = v0[c] + v1[c] - v2[c];
    }
  } else if (d02 >= d12) {
    for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
      v3[c] = v0[c] + v2[c] - v1[c];
    }
  } else {
    for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
      v3[c] = v1[c] + v2[c] - v0[c];
    }
  }
  auto write = [&](uint32_t i, const std::vector<float>& src) {
    std::memcpy(vert(i), src.data(), kVertexStrideBytes);
  };
  write(0, v0);
  write(1, v1);
  write(2, v2);
  write(3, v0);
  write(4, v3);
  write(5, v2);
  return 6;
}

void UnpackVertex(rex::graphics::xenos::VertexFormat format, const uint32_t data[4], bool is_signed,
                  bool is_normalized, float out[4]) {
  using rex::graphics::xenos::VertexFormat;
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  switch (format) {
    case VertexFormat::k_32_FLOAT:
    case VertexFormat::k_32_32_FLOAT:
    case VertexFormat::k_32_32_32_FLOAT:
    case VertexFormat::k_32_32_32_32_FLOAT:
      std::memcpy(out, data, 16);
      if (format == VertexFormat::k_32_FLOAT) {
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
      } else if (format == VertexFormat::k_32_32_FLOAT) {
        out[2] = 0.0f;
        out[3] = 1.0f;
      } else if (format == VertexFormat::k_32_32_32_FLOAT) {
        out[3] = 1.0f;
      }
      break;
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_16_16_16_16_FLOAT:
      out[0] = rex::xenos_half_to_float(uint16_t(data[0]));
      out[1] = rex::xenos_half_to_float(uint16_t(data[0] >> 16));
      if (format == VertexFormat::k_16_16_16_16_FLOAT) {
        out[2] = rex::xenos_half_to_float(uint16_t(data[1]));
        out[3] = rex::xenos_half_to_float(uint16_t(data[1] >> 16));
      }
      break;
    case VertexFormat::k_8_8_8_8: {
      const float scale = is_signed ? (is_normalized ? (1.0f / 127.0f) : 1.0f)
                                    : (is_normalized ? (1.0f / 255.0f) : 1.0f);
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t byte = (data[0] >> (8 * i)) & 0xFF;
        if (is_signed) {
          out[i] = float(int8_t(byte)) * scale;
        } else {
          out[i] = float(byte) * scale;
        }
      }
      break;
    }
    case VertexFormat::k_32:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_32_32:
      for (uint32_t i = 0; i < 4; ++i) {
        if (is_signed) {
          out[i] = float(int32_t(data[i]));
          if (is_normalized) {
            out[i] /= 2147483647.0f;
          }
        } else {
          out[i] = float(data[i]);
          if (is_normalized) {
            out[i] /= 4294967295.0f;
          }
        }
      }
      break;
    default:
      std::memcpy(out, data, 16);
      break;
  }
}

rex::graphics::xenos::xe_gpu_vertex_fetch_t FetchAt(const GuestDrawSnapshot& snap, uint32_t index) {
  rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch{};
  if (index < 96) {
    std::memcpy(&fetch, &snap.fetch_constants[index * 2], sizeof(fetch));
  }
  return fetch;
}

rex::graphics::xenos::xe_gpu_texture_fetch_t TextureFetchAt(const GuestDrawSnapshot& snap,
                                                            uint32_t slot) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (slot < rex::graphics::xenos::kTextureFetchConstantCount) {
    std::memcpy(&fetch, &snap.fetch_constants[slot * 6], sizeof(fetch));
  }
  return fetch;
}

uint64_t TextureKey(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch) {
  return (uint64_t(fetch.base_address) << 32) ^ (uint64_t(fetch.format) << 16) ^
         uint64_t(fetch.dword_2);
}

bool MapHostFormat(rex::graphics::xenos::TextureFormat format, plume::RenderFormat* host,
                   bool* expand_r8) {
  using rex::graphics::xenos::TextureFormat;
  format = rex::graphics::GetBaseFormat(format);
  *expand_r8 = false;
  switch (format) {
    case TextureFormat::k_DXT1:
      *host = plume::RenderFormat::BC1_UNORM;
      return true;
    case TextureFormat::k_DXT2_3:
      *host = plume::RenderFormat::BC2_UNORM;
      return true;
    case TextureFormat::k_DXT4_5:
      *host = plume::RenderFormat::BC3_UNORM;
      return true;
    case TextureFormat::k_8_8_8_8:
      *host = plume::RenderFormat::R8G8B8A8_UNORM;
      return true;
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
      *host = plume::RenderFormat::R8G8B8A8_UNORM;
      *expand_r8 = true;
      return true;
    default:
      return false;
  }
}

bool UntileGuestTexture(const rex::graphics::TextureInfo& info, const uint8_t* src,
                        std::vector<uint8_t>* out, uint32_t* row_bytes_out,
                        uint32_t* row_texels_out, bool expand_r8) {
  const rex::graphics::FormatInfo* fi = info.format_info();
  if (!fi || !src || !out) {
    return false;
  }
  const uint32_t width = info.width + 1;
  const uint32_t height = info.height + 1;
  const uint32_t bpb = fi->bytes_per_block();
  if (bpb == 0 || fi->block_width == 0 || fi->block_height == 0) {
    return false;
  }
  const uint32_t bpb_log2 = rex::log2_floor(bpb);
  const uint32_t block_w = rex::align(width, fi->block_width) / fi->block_width;
  const uint32_t block_h = rex::align(height, fi->block_height) / fi->block_height;
  const uint32_t aligned_w = rex::align(block_w, 32u);
  const uint32_t row_bytes = rex::align(block_w * bpb, 256u);
  const uint32_t row_texels = (row_bytes / bpb) * fi->block_width;
  int pack_x = 0;
  int pack_y = 0;
  if (info.has_packed_mips) {
    uint32_t px = 0;
    uint32_t py = 0;
    uint32_t pz = 0;
    rex::graphics::texture_util::GetPackedMipOffset(width, height, 1, info.format, 0, px, py, pz);
    pack_x = int(px);
    pack_y = int(py);
  }

  std::vector<uint8_t> blocks(size_t(row_bytes) * block_h, 0);
  if (info.is_tiled) {
    const uint32_t tiled_size = aligned_w * rex::align(block_h, 32u) * bpb;
    for (uint32_t by = 0; by < block_h; ++by) {
      for (uint32_t bx = 0; bx < block_w; ++bx) {
        const int32_t so = rex::graphics::texture_util::GetTiledOffset2D(
            pack_x + int32_t(bx), pack_y + int32_t(by), aligned_w, bpb_log2);
        if (so < 0 || uint32_t(so) + bpb > tiled_size) {
          continue;
        }
        rex::graphics::texture_conversion::CopySwapBlock(
            info.endianness, blocks.data() + size_t(by) * row_bytes + size_t(bx) * bpb, src + so,
            bpb);
      }
    }
  } else {
    const uint32_t pitch_blocks = std::max(1u, info.pitch / fi->block_width);
    const uint32_t src_row = rex::align(pitch_blocks * bpb, 256u);
    for (uint32_t by = 0; by < block_h; ++by) {
      for (uint32_t bx = 0; bx < block_w; ++bx) {
        const uint32_t so = by * src_row + bx * bpb;
        rex::graphics::texture_conversion::CopySwapBlock(
            info.endianness, blocks.data() + size_t(by) * row_bytes + size_t(bx) * bpb, src + so,
            bpb);
      }
    }
  }

  if (expand_r8) {
    const uint32_t dst_row = rex::align(width * 4u, 256u);
    out->assign(size_t(dst_row) * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t r = blocks[size_t(y) * row_bytes + x];
        uint8_t* p = out->data() + size_t(y) * dst_row + size_t(x) * 4;
        p[0] = p[1] = p[2] = p[3] = r;
      }
    }
    *row_bytes_out = dst_row;
    *row_texels_out = dst_row / 4;
    return true;
  }

  *out = std::move(blocks);
  *row_bytes_out = row_bytes;
  *row_texels_out = row_texels;
  return true;
}

}  // namespace

PlumeDrawContext::~PlumeDrawContext() {
  Shutdown();
}

void PlumeDrawContext::Shutdown() {
  ready_ = false;
  null_texture_uploaded_ = false;
  pipelines_.clear();
  {
    std::lock_guard lock(vfetch_mutex_);
    vfetch_by_shader_.clear();
  }
  if (vs_constants_ && vs_constants_mapped_) {
    vs_constants_->unmap();
  }
  if (ps_constants_ && ps_constants_mapped_) {
    ps_constants_->unmap();
  }
  if (shared_constants_ && shared_constants_mapped_) {
    shared_constants_->unmap();
  }
  if (dummy_vb_ && vb_mapped_) {
    dummy_vb_->unmap();
  }
  vs_constants_mapped_ = nullptr;
  ps_constants_mapped_ = nullptr;
  shared_constants_mapped_ = nullptr;
  vb_mapped_ = nullptr;
  vs_constants_addr_ = 0;
  ps_constants_addr_ = 0;
  shared_constants_addr_ = 0;
  vs_constants_.reset();
  ps_constants_.reset();
  shared_constants_.reset();
  dummy_vb_.reset();
  guest_textures_.clear();
  next_bindless_ = 1;
  null_texture_staging_.reset();
  null_texture_view_.reset();
  null_texture_.reset();
  passthrough_tri_.reset();
  passthrough_point_.reset();
  passthrough_fan_.reset();
  passthrough_strip_.reset();
  passthrough_vs_.reset();
  passthrough_ps_.reset();
  sampler_.reset();
  sampler_set_.reset();
  texture_set_.reset();
  pipeline_layout_.reset();
  device_ = nullptr;
}

bool PlumeDrawContext::Initialize(plume::RenderDevice* device) {
  Shutdown();
  if (!device) {
    return false;
  }
  device_ = device;

  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);

  plume::RenderDescriptorSetBuilder tex_set_builder;
  tex_set_builder.begin();
  tex_set_builder.addTexture(0, kBindlessTextureCount);
  tex_set_builder.end(true, kBindlessTextureCount);
  texture_set_ = tex_set_builder.create(device_);
  if (!texture_set_) {
    REXLOG_ERROR("plume: draw texture descriptor set failed");
    Shutdown();
    return false;
  }

  layout_builder.addDescriptorSet(tex_set_builder);
  layout_builder.addDescriptorSet(tex_set_builder);
  layout_builder.addDescriptorSet(tex_set_builder);

  plume::RenderDescriptorSetBuilder sampler_set_builder;
  sampler_set_builder.begin();
  sampler_set_builder.addSampler(0, kBindlessSamplerCount);
  sampler_set_builder.end(true, kBindlessSamplerCount);
  sampler_set_ = sampler_set_builder.create(device_);
  if (!sampler_set_) {
    REXLOG_ERROR("plume: draw sampler descriptor set failed");
    Shutdown();
    return false;
  }
  layout_builder.addDescriptorSet(sampler_set_builder);

  layout_builder.addPushConstant(0, 4, kPushConstantBytes,
                                 plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  layout_builder.end();
  pipeline_layout_ = layout_builder.create(device_);
  if (!pipeline_layout_) {
    REXLOG_ERROR("plume: draw pipeline layout failed");
    Shutdown();
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::NEAREST;
  sampler_desc.magFilter = plume::RenderFilter::NEAREST;
  sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  sampler_ = device_->createSampler(sampler_desc);
  if (!sampler_) {
    REXLOG_ERROR("plume: draw sampler failed");
    Shutdown();
    return false;
  }
  for (uint32_t i = 0; i < kBindlessSamplerCount; ++i) {
    sampler_set_->setSampler(i, sampler_.get());
  }

  plume::RenderTextureDesc tex_desc = plume::RenderTextureDesc::Texture2D(
      1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM);
  tex_desc.committed = true;
  null_texture_ = device_->createTexture(tex_desc);
  if (!null_texture_) {
    REXLOG_ERROR("plume: draw null texture failed");
    Shutdown();
    return false;
  }
  null_texture_view_ =
      null_texture_->createTextureView(plume::RenderTextureViewDesc::Texture2D(tex_desc.format));
  if (!null_texture_view_) {
    REXLOG_ERROR("plume: draw null texture view failed");
    Shutdown();
    return false;
  }
  for (uint32_t i = 0; i < kBindlessTextureCount; ++i) {
    texture_set_->setTexture(i, null_texture_.get(), plume::RenderTextureLayout::SHADER_READ,
                             null_texture_view_.get());
  }

  null_texture_staging_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (null_texture_staging_) {
    if (void* pixels = null_texture_staging_->map()) {
      const uint8_t white[4] = {255, 255, 255, 255};
      std::memcpy(pixels, white, sizeof(white));
      null_texture_staging_->unmap();
    }
  }

  auto make_cb = [&](uint64_t size, const char* tag) -> std::unique_ptr<plume::RenderBuffer> {
    auto buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        size, plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!buffer || !VulkanBufferOk(buffer.get())) {
      REXLOG_ERROR("plume: draw {} buffer failed", tag);
      return nullptr;
    }
    return buffer;
  };
  vs_constants_ = make_cb(uint64_t(kVsSlotBytes) * kCbSlots, "VS constants");
  ps_constants_ = make_cb(uint64_t(kPsSlotBytes) * kCbSlots, "PS constants");
  shared_constants_ = make_cb(uint64_t(kSharedSlotBytes) * kCbSlots, "shared constants");
  if (!vs_constants_ || !ps_constants_ || !shared_constants_) {
    Shutdown();
    return false;
  }

  vs_constants_mapped_ = vs_constants_->map();
  ps_constants_mapped_ = ps_constants_->map();
  shared_constants_mapped_ = shared_constants_->map();
  if (!vs_constants_mapped_ || !ps_constants_mapped_ || !shared_constants_mapped_) {
    REXLOG_ERROR("plume: draw constant map failed");
    Shutdown();
    return false;
  }
  std::memset(vs_constants_mapped_, 0, size_t(kVsSlotBytes) * kCbSlots);
  std::memset(ps_constants_mapped_, 0, size_t(kPsSlotBytes) * kCbSlots);
  std::memset(shared_constants_mapped_, 0, size_t(kSharedSlotBytes) * kCbSlots);
  vs_constants_addr_ = vs_constants_->getDeviceAddress();
  ps_constants_addr_ = ps_constants_->getDeviceAddress();
  shared_constants_addr_ = shared_constants_->getDeviceAddress();

  dummy_vb_ = device_->createBuffer(plume::RenderBufferDesc::VertexBuffer(
      uint64_t(kDummyVertexCount) * kVertexStrideBytes, plume::RenderHeapType::UPLOAD));
  if (!dummy_vb_ || !VulkanBufferOk(dummy_vb_.get())) {
    REXLOG_ERROR("plume: draw vertex buffer failed");
    Shutdown();
    return false;
  }
  vb_mapped_ = static_cast<float*>(dummy_vb_->map());
  if (!vb_mapped_) {
    REXLOG_ERROR("plume: draw vertex buffer map failed");
    Shutdown();
    return false;
  }
  vb_view_ = plume::RenderVertexBufferView(dummy_vb_.get(), kDummyVertexCount * kVertexStrideBytes);

  input_slots_[0] = plume::RenderInputSlot(0, kVertexStrideBytes,
                                           plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  for (uint32_t i = 0; i < kInputLocationCount; ++i) {
    input_elements_[i] = plume::RenderInputElement(
        "TEXCOORD", i, i, plume::RenderFormat::R32G32B32A32_FLOAT, 0, i * 16);
  }

  ready_ = true;
  if (!CreatePassthroughPipeline()) {
    REXLOG_ERROR("plume: passthrough pipeline failed");
    Shutdown();
    return false;
  }
  REXLOG_INFO("plume: draw context ready (bindless {} tex / {} samp, VB {} verts)",
              kBindlessTextureCount, kBindlessSamplerCount, kDummyVertexCount);
  return true;
}

bool PlumeDrawContext::CreatePassthroughPipeline() {
  passthrough_vs_ = device_->createShader(kPassthroughVsSpirv, kPassthroughVsSpirv_count * 4,
                                          "main", plume::RenderShaderFormat::SPIRV);
  passthrough_ps_ = device_->createShader(kPassthroughPsSpirv, kPassthroughPsSpirv_count * 4,
                                          "main", plume::RenderShaderFormat::SPIRV);
  if (!passthrough_vs_ || !passthrough_ps_) {
    return false;
  }

  auto make_pipe = [&](plume::RenderPrimitiveTopology topology)
      -> std::unique_ptr<plume::RenderPipeline> {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = pipeline_layout_.get();
    desc.vertexShader = passthrough_vs_.get();
    desc.pixelShader = passthrough_ps_.get();
    desc.primitiveTopology = topology;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.fillMode = plume::RenderFillMode::SOLID;
    desc.frontFace = plume::RenderFrontFace::CLOCKWISE;
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    desc.depthClipEnabled = false;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
    desc.inputSlots = &input_slots_[0];
    desc.inputSlotsCount = 1;
    desc.inputElements = input_elements_.data();
    desc.inputElementsCount = kInputLocationCount;
    auto pipeline = device_->createGraphicsPipeline(desc);
    if (!pipeline || !VulkanPipelineOk(pipeline.get())) {
      return nullptr;
    }
    return pipeline;
  };

  passthrough_tri_ = make_pipe(plume::RenderPrimitiveTopology::TRIANGLE_LIST);
  passthrough_point_ = make_pipe(plume::RenderPrimitiveTopology::POINT_LIST);
  passthrough_fan_ = make_pipe(plume::RenderPrimitiveTopology::TRIANGLE_FAN);
  passthrough_strip_ = make_pipe(plume::RenderPrimitiveTopology::TRIANGLE_STRIP);
  return passthrough_tri_ && passthrough_point_ && passthrough_fan_ && passthrough_strip_;
}

plume::RenderPipeline* PlumeDrawContext::PassthroughFor(plume::RenderPrimitiveTopology topology) {
  if (topology == plume::RenderPrimitiveTopology::POINT_LIST) {
    return passthrough_point_.get();
  }
  if (topology == plume::RenderPrimitiveTopology::TRIANGLE_FAN) {
    return passthrough_fan_.get();
  }
  if (topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP) {
    return passthrough_strip_.get();
  }
  return passthrough_tri_.get();
}

void PlumeDrawContext::RegisterVsUcode(uint64_t shader_hash, const uint8_t* bytes,
                                       uint32_t byte_size) {
  if (!bytes || byte_size == 0 || shader_hash == 0) {
    return;
  }
  std::vector<VfetchAttr> attrs = ParseVfetches(bytes, byte_size);
  std::lock_guard lock(vfetch_mutex_);
  if (vfetch_by_shader_.contains(shader_hash)) {
    return;
  }
  REXLOG_INFO("plume: VS {:016X} parsed {} vfetch attr(s)", shader_hash, attrs.size());
  for (size_t i = 0; i < attrs.size() && i < 4; ++i) {
    const VfetchAttr& a = attrs[i];
    REXLOG_INFO("plume:   attr{} fetch={} stride={} off={} fmt={}", a.location, a.fetch_const,
                a.stride_dwords, a.offset_dwords, a.format);
  }
  vfetch_by_shader_.emplace(shader_hash, std::move(attrs));
}

plume::RenderPrimitiveTopology PlumeDrawContext::MapTopology(uint32_t prim_type) const {
  using rex::graphics::xenos::PrimitiveType;
  using plume::RenderPrimitiveTopology;
  switch (static_cast<PrimitiveType>(prim_type)) {
    case PrimitiveType::kPointList:
      return RenderPrimitiveTopology::POINT_LIST;
    case PrimitiveType::kLineList:
      return RenderPrimitiveTopology::LINE_LIST;
    case PrimitiveType::kLineStrip:
    case PrimitiveType::kLineLoop:
      return RenderPrimitiveTopology::LINE_STRIP;
    case PrimitiveType::kTriangleStrip:
    case PrimitiveType::kQuadStrip:
      return RenderPrimitiveTopology::TRIANGLE_STRIP;
    case PrimitiveType::kTriangleFan:
    case PrimitiveType::kPolygon:
      return RenderPrimitiveTopology::TRIANGLE_FAN;
    case PrimitiveType::kRectangleList:
    case PrimitiveType::kQuadList:
      return RenderPrimitiveTopology::TRIANGLE_LIST;
    default:
      return RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

plume::RenderPipeline* PlumeDrawContext::GetOrCreatePipeline(
    uint64_t vs_hash, uint64_t ps_hash, plume::RenderPrimitiveTopology topology) {
  PipelineKey key{vs_hash, ps_hash, uint32_t(topology)};
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second.get();
  }

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  plume::RenderShader* vs = cache.GetOrCreateShader(vs_hash);
  plume::RenderShader* ps = cache.GetOrCreateShader(ps_hash);
  if (!vs || !ps) {
    static uint32_t miss_logs = 0;
    if (miss_logs < 8) {
      ++miss_logs;
      REXLOG_WARN("plume: draw pipeline skipped vs={:016X} ({}) ps={:016X} ({})", vs_hash,
                  vs ? "ok" : "miss", ps_hash, ps ? "ok" : "miss");
    }
    return nullptr;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = pipeline_layout_.get();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.primitiveTopology = topology;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.frontFace = plume::RenderFrontFace::CLOCKWISE;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.depthClipEnabled = false;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  desc.inputSlots = &input_slots_[0];
  desc.inputSlotsCount = 1;
  desc.inputElements = input_elements_.data();
  desc.inputElementsCount = kInputLocationCount;

  std::unique_ptr<plume::RenderPipeline> pipeline = device_->createGraphicsPipeline(desc);
  if (!pipeline || !VulkanPipelineOk(pipeline.get())) {
    static uint32_t fail_logs = 0;
    if (fail_logs < 8) {
      ++fail_logs;
      REXLOG_ERROR("plume: createGraphicsPipeline failed vs={:016X} ps={:016X} topo={}", vs_hash,
                   ps_hash, uint32_t(topology));
    }
    pipelines_.emplace(key, nullptr);
    return nullptr;
  }

  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  static uint32_t ok_logs = 0;
  if (ok_logs < 8) {
    ++ok_logs;
    REXLOG_INFO("plume: created graphics pipeline vs={:016X} ps={:016X} topo={}", vs_hash, ps_hash,
                uint32_t(topology));
  }
  return raw;
}

uint32_t PlumeDrawContext::FillVertices(const GuestDrawSnapshot& snap, memory::Memory* memory,
                                        uint32_t base_vertex, bool passthrough) {
  last_fill_passthrough_ = passthrough;
  if (!vb_mapped_ || snap.num_indices == 0 || base_vertex >= kDummyVertexCount) {
    return 0;
  }
  const uint32_t vertex_count = std::min(snap.num_indices, kDummyVertexCount - base_vertex);

  std::vector<VfetchAttr> attrs;
  {
    std::lock_guard lock(vfetch_mutex_);
    if (auto it = vfetch_by_shader_.find(snap.vs_hash); it != vfetch_by_shader_.end()) {
      attrs = it->second;
    }
  }

  auto vert = [&](uint32_t i) { return vb_mapped_ + (base_vertex + i) * kFloatsPerVert; };
  std::memset(vert(0), 0, size_t(vertex_count) * kVertexStrideBytes);

  if (attrs.empty() || !memory) {
    for (uint32_t i = 0; i < vertex_count; ++i) {
      const uint32_t corner = i % 3;
      float* dst = vert(i);
      dst[0] = corner == 1 ? 3.0f : -1.0f;
      dst[1] = corner == 2 ? 3.0f : -1.0f;
      dst[2] = 0.0f;
      dst[3] = 1.0f;
    }
    last_fill_passthrough_ = true;
    return vertex_count;
  }

  using rex::graphics::xenos::FetchConstantType;
  using rex::graphics::xenos::PrimitiveType;
  uint32_t fetched = 0;
  uint32_t fetch_addr = 0;
  uint32_t fetch_type = 0;
  for (uint32_t vi = 0; vi < vertex_count; ++vi) {
    float* dst = vert(vi);
    for (const VfetchAttr& attr : attrs) {
      const auto fetch = FetchAt(snap, attr.fetch_const);
      if (fetch.type != FetchConstantType::kVertex &&
          fetch.type != FetchConstantType::kInvalidVertex) {
        continue;
      }
      if (vi == 0 && attr.location == 0) {
        fetch_addr = fetch.address;
        fetch_type = uint32_t(fetch.type);
      }
      const int32_t dword_addr =
          int32_t(fetch.address) + int32_t(attr.stride_dwords * vi) + attr.offset_dwords;
      if (dword_addr < int32_t(fetch.address)) {
        continue;
      }
      uint32_t data[4] = {};
      for (uint32_t w = 0; w < 4; ++w) {
        const uint32_t phys = uint32_t(dword_addr + int32_t(w)) << 2;
        if (auto* host = memory->TranslatePhysical<uint32_t*>(phys)) {
          data[w] = rex::graphics::xenos::GpuSwap(*host, fetch.endian);
        }
      }
      float unpacked[4];
      UnpackVertex(static_cast<rex::graphics::xenos::VertexFormat>(attr.format), data,
                   attr.is_signed, attr.is_normalized, unpacked);
      if (attr.exp_adjust != 0) {
        const float scale = std::ldexp(1.0f, attr.exp_adjust);
        unpacked[0] *= scale;
        unpacked[1] *= scale;
        unpacked[2] *= scale;
        unpacked[3] *= scale;
      }
      std::memcpy(dst + attr.location * 4, unpacked, 16);
      if (attr.location == 0) {
        ++fetched;
      }
    }
  }

  const float* c0 = reinterpret_cast<const float*>(snap.vs_constants.data());
  const float* ps0 = reinterpret_cast<const float*>(snap.ps_constants.data());
  bool looks_pixel = false;
  for (uint32_t vi = 0; vi < vertex_count; ++vi) {
    const float* pos = vert(vi);
    if ((std::fabs(pos[0]) > 2.0f || std::fabs(pos[1]) > 2.0f) && std::fabs(pos[2]) < 2.0f) {
      looks_pixel = true;
      break;
    }
  }

  if (passthrough && static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kRectangleList &&
      attrs.size() <= 2) {
    // Position-only / UV-only fullscreen blits. Without guest textures they
    // paint a solid quad over the actual UI.
    return 0;
  }
  if (passthrough && looks_pixel) {
    float xscale = snap.vport_xscale;
    float xoffset = snap.vport_xoffset;
    float yscale = snap.vport_yscale;
    float yoffset = snap.vport_yoffset;
    if (std::fabs(xscale) < 1e-6f) {
      xscale = 640.0f;
      xoffset = 640.0f;
    }
    if (std::fabs(yscale) < 1e-6f) {
      yscale = -360.0f;
      yoffset = 360.0f;
    }
    for (uint32_t vi = 0; vi < vertex_count; ++vi) {
      float* pos = vert(vi);
      pos[0] = (pos[0] - xoffset) / xscale;
      pos[1] = (pos[1] - yoffset) / yscale;
      if (pos[3] == 0.0f) {
        pos[3] = 1.0f;
      }
    }
  }

  uint32_t draw_count = vertex_count;
  if (static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kRectangleList &&
      vertex_count >= 3) {
    draw_count = ExpandRectangle(vb_mapped_, base_vertex, vertex_count);
  }

  if (passthrough) {
    for (uint32_t vi = 0; vi < draw_count; ++vi) {
      float* pos = vert(vi);
      pos[1] = -pos[1];
    }
    const float texid = float(BindlessForTexture(snap));
    bool has_color_attr = false;
    for (const VfetchAttr& attr : attrs) {
      if (attr.location == 17) {
        has_color_attr = true;
        break;
      }
    }
    const bool ps_live = !Float4Dead(ps0);
    for (uint32_t vi = 0; vi < draw_count; ++vi) {
      float* dst = vert(vi);
      dst[4] = texid;
      dst[5] = 0.0f;
      dst[6] = 0.0f;
      dst[7] = 1.0f;
      if (!has_color_attr && ps_live) {
        dst[17 * 4 + 0] = ps0[0];
        dst[17 * 4 + 1] = ps0[1];
        dst[17 * 4 + 2] = ps0[2];
        dst[17 * 4 + 3] = ps0[3] == 0.0f ? 1.0f : ps0[3];
      }
    }
  }

  static uint32_t fetch_logs = 0;
  if (fetch_logs < 16 && vertex_count > 0) {
    ++fetch_logs;
    const float* p0 = vert(0);
    const float* c17 = p0 + 17 * 4;
    REXLOG_INFO(
        "plume: fetched {} verts loc0=({:.3f},{:.3f},{:.3f},{:.3f}) loc17=({:.3f},{:.3f},{:.3f}) "
        "attrs={} prim={} fetch={:08X}/t{} c0=({:.4f},{:.4f},{:.4f},{:.4f}) "
        "ps0=({:.4f},{:.4f},{:.4f},{:.4f}) vte={:08X} ndc={} pass={}",
        draw_count, p0[0], p0[1], p0[2], p0[3], c17[0], c17[1], c17[2], attrs.size(), snap.prim_type,
        fetch_addr, fetch_type, c0[0], c0[1], c0[2], c0[3], ps0[0], ps0[1], ps0[2], ps0[3],
        snap.vte_cntl, looks_pixel ? 1 : 0, last_fill_passthrough_ ? 1 : 0);
  }
  (void)fetched;
  return draw_count;
}

void PlumeDrawContext::UploadNullTexture(plume::RenderCommandList* list) {
  if (null_texture_uploaded_ || !list || !null_texture_ || !null_texture_staging_) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(null_texture_.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  const plume::RenderTextureCopyLocation src = plume::RenderTextureCopyLocation::PlacedFootprint(
      null_texture_staging_.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
  const plume::RenderTextureCopyLocation dst =
      plume::RenderTextureCopyLocation::Subresource(null_texture_.get(), 0, 0);
  list->copyTextureRegion(dst, src, 0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(null_texture_.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  null_texture_uploaded_ = true;
}

void PlumeDrawContext::BindGuestTextures(plume::RenderCommandList* list,
                                         const std::vector<GuestDrawSnapshot>& draws,
                                         memory::Memory* memory) {
  if (!list || !memory || !texture_set_ || !device_) {
    return;
  }
  using rex::graphics::xenos::FetchConstantType;
  for (const GuestDrawSnapshot& snap : draws) {
    if (!snap.valid) {
      continue;
    }
    for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
      const auto fetch = TextureFetchAt(snap, slot);
      if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
        continue;
      }
      plume::RenderFormat host_format = plume::RenderFormat::R8G8B8A8_UNORM;
      bool expand_r8 = false;
      if (!MapHostFormat(fetch.format, &host_format, &expand_r8)) {
        continue;
      }
      const uint64_t key = TextureKey(fetch);
      if (guest_textures_.contains(key)) {
        continue;
      }
      if (next_bindless_ >= kBindlessTextureCount) {
        return;
      }
      rex::graphics::TextureInfo info{};
      if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
        continue;
      }
      const uint32_t width = info.width + 1;
      const uint32_t height = info.height + 1;
      if (width == 0 || height == 0 || width > 2048 || height > 2048) {
        continue;
      }
      const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(info.memory.base_address);
      if (!src) {
        continue;
      }
      std::vector<uint8_t> pixels;
      uint32_t row_bytes = 0;
      uint32_t row_texels = 0;
      if (!UntileGuestTexture(info, src, &pixels, &row_bytes, &row_texels, expand_r8) ||
          pixels.empty() || row_bytes == 0) {
        continue;
      }

      auto host = std::make_unique<GuestHostTexture>();
      host->key = key;
      host->bindless = next_bindless_++;
      host->width = width;
      host->height = height;
      host->format = host_format;
      plume::RenderTextureDesc tex_desc =
          plume::RenderTextureDesc::Texture2D(width, height, 1, host_format);
      tex_desc.committed = true;
      host->texture = device_->createTexture(tex_desc);
      if (!host->texture) {
        continue;
      }
      host->view =
          host->texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(host_format));
      if (!host->view) {
        continue;
      }
      host->staging = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(pixels.size()));
      if (!host->staging) {
        continue;
      }
      if (void* mapped = host->staging->map()) {
        std::memcpy(mapped, pixels.data(), pixels.size());
        host->staging->unmap();
      }

      list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(host->texture.get(),
                                                 plume::RenderTextureLayout::COPY_DEST));
      const plume::RenderTextureCopyLocation copy_src =
          plume::RenderTextureCopyLocation::PlacedFootprint(
              host->staging.get(), host_format, width, height, 1, row_texels, 0);
      const plume::RenderTextureCopyLocation copy_dst =
          plume::RenderTextureCopyLocation::Subresource(host->texture.get(), 0, 0);
      list->copyTextureRegion(copy_dst, copy_src, 0, 0, 0, nullptr);
      list->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(host->texture.get(),
                                                 plume::RenderTextureLayout::SHADER_READ));
      texture_set_->setTexture(host->bindless, host->texture.get(),
                               plume::RenderTextureLayout::SHADER_READ, host->view.get());
      host->uploaded = true;

      static uint32_t tex_logs = 0;
      if (tex_logs < 8) {
        ++tex_logs;
        REXLOG_INFO("plume: guest tex slot={} {}x{} fmt={} tiled={} addr={:08X} bindless={}", slot,
                    width, height, uint32_t(fetch.format), info.is_tiled ? 1 : 0,
                    info.memory.base_address, host->bindless);
      }
      guest_textures_.emplace(key, std::move(host));
    }
  }
}

uint32_t PlumeDrawContext::BindlessForTexture(const GuestDrawSnapshot& snap) const {
  using rex::graphics::xenos::FetchConstantType;
  for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
    const uint32_t bindless = BindlessForSlot(snap, slot);
    if (bindless != 0) {
      return bindless;
    }
  }
  return 0;
}

uint32_t PlumeDrawContext::BindlessForSlot(const GuestDrawSnapshot& snap, uint32_t slot) const {
  using rex::graphics::xenos::FetchConstantType;
  const auto fetch = TextureFetchAt(snap, slot);
  if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
    return 0;
  }
  const uint64_t key = TextureKey(fetch);
  if (auto it = guest_textures_.find(key); it != guest_textures_.end() && it->second) {
    return it->second->bindless;
  }
  return 0;
}

void PlumeDrawContext::FillSharedConstants(uint8_t* dst, const GuestDrawSnapshot& snap,
                                           uint32_t width, uint32_t height) const {
  if (!dst) {
    return;
  }
  std::memset(dst, 0, kSharedConstantBytes);
  auto* words = reinterpret_cast<uint32_t*>(dst);
  for (uint32_t slot = 0; slot < 16; ++slot) {
    words[slot] = BindlessForSlot(snap, slot);
    words[48 + slot] = 0;
  }
  words[64] = snap.vs_bool;
  float half_x = width ? 1.0f / float(width) : (1.0f / 1280.0f);
  float half_y = height ? 1.0f / float(height) : (1.0f / 720.0f);
  std::memcpy(dst + 280, &half_x, 4);
  std::memcpy(dst + 284, &half_y, 4);
  float alpha = 0.0f;
  std::memcpy(dst + 308, &alpha, 4);
}

void PlumeDrawContext::EncodeDraws(plume::RenderCommandList* list,
                                   const std::vector<GuestDrawSnapshot>& draws,
                                   memory::Memory* memory, uint32_t width, uint32_t height) {
  if (!ready_ || !list || draws.empty()) {
    return;
  }

  UploadNullTexture(list);
  BindGuestTextures(list, draws, memory);

  list->setGraphicsPipelineLayout(pipeline_layout_.get());
  list->setGraphicsDescriptorSet(texture_set_.get(), 0);
  list->setGraphicsDescriptorSet(texture_set_.get(), 1);
  list->setGraphicsDescriptorSet(texture_set_.get(), 2);
  list->setGraphicsDescriptorSet(sampler_set_.get(), 3);

  uint32_t encoded = 0;
  uint32_t vb_used = 0;
  uint32_t guest_draws = 0;
  using rex::graphics::xenos::PrimitiveType;
  for (const GuestDrawSnapshot& snap : draws) {
    if (!snap.valid || snap.vs_hash == 0 || snap.ps_hash == 0 || snap.num_indices == 0) {
      continue;
    }
    if (static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kPointList &&
        snap.num_indices < 3) {
      continue;
    }
    if (vb_used >= kDummyVertexCount || encoded >= kCbSlots) {
      break;
    }
    const plume::RenderPrimitiveTopology topology = MapTopology(snap.prim_type);
    // Guest VS/PS still miss PS ALU and SharedConstants texture IDs. Pixel-space
    // UI with only VS c0=(-1,1,0,1) was sent to guest shaders and clipped, leaving
    // a lone NDC origin point. Stay on passthrough until the D3D shadow is wired.
    const bool passthrough = true;
    plume::RenderPipeline* pipeline = PassthroughFor(topology);
    if (!pipeline) {
      continue;
    }
    const uint32_t vertex_count = FillVertices(snap, memory, vb_used, passthrough);
    if (vertex_count == 0) {
      continue;
    }

    auto* vs_dst = static_cast<uint8_t*>(vs_constants_mapped_) + encoded * kVsSlotBytes;
    auto* ps_dst = static_cast<uint8_t*>(ps_constants_mapped_) + encoded * kPsSlotBytes;
    auto* shared_dst = static_cast<uint8_t*>(shared_constants_mapped_) + encoded * kSharedSlotBytes;
    std::memcpy(vs_dst, snap.vs_constants.data(), kVsConstantBytes);
    std::memcpy(ps_dst, snap.ps_constants.data(), kPsConstantBytes);
    FillSharedConstants(shared_dst, snap, width, height);

    const uint64_t push[3] = {vs_constants_addr_ + encoded * kVsSlotBytes,
                              ps_constants_addr_ + encoded * kPsSlotBytes,
                              shared_constants_addr_ + encoded * kSharedSlotBytes};
    list->setGraphicsPushConstants(0, push, 0, kPushConstantBytes);
    list->setPipeline(pipeline);
    list->setVertexBuffers(0, &vb_view_, 1, &input_slots_[0]);
    list->drawInstanced(vertex_count, 1, vb_used, 0);
    vb_used += vertex_count;
    ++encoded;
  }

  static uint32_t encode_logs = 0;
  if (encode_logs < 8 && encoded > 0) {
    ++encode_logs;
    const GuestDrawSnapshot& last = draws.back();
    REXLOG_INFO(
        "plume: encoded {}/{} DRAWs ({} guest) last vs={:016X} ps={:016X} prim={} indices={}",
        encoded, draws.size(), guest_draws, last.vs_hash, last.ps_hash, last.prim_type,
        last.num_indices);
  }
}

}  // namespace rex::plume_renderer
