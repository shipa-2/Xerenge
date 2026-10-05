#include <atomic>
#include <mutex>
#include <string>
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <execinfo.h>
#endif
#ifdef __ANDROID__
#include <malloc.h>
#endif
#include <thread>
/**
 * @file        plume_renderer/plume_draw.cpp
 * @brief       Guest DRAW_INDX_2 → plume graphics pipeline (present thread).
 */
#include "plume_renderer/plume_draw.h"
#include "diagnostics.h"
#include "plume_renderer/plume_parallel.h"

#include <algorithm>
#include <functional>
#include <bit>
#include <map>
#include <set>
#include <tuple>
#include <cstring>
#include <cmath>
#include <utility>
#include <vector>
#include <unordered_set>

#include <plume_render_interface_builders.h>
#include <plume_vulkan.h>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>

#include "plume_renderer/plume_passthrough_spirv.h"
#include "plume_renderer/plume_video_spirv.h"
#include "plume_renderer/plume_shader_cache.h"
#include "shader_cache.h"
#include "plume_renderer/plume_swapchain.h"
#include "plume_renderer/shader_source_info.h"

#include <xxhash.h>

#include <ProcessRGB.hpp>

#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <ctime>

namespace rex::plume_renderer {

namespace {
void TrapArm(uint32_t physical_byte_address);  // the vertex writer trap, defined below
void CheckShaderConstants(const GuestDrawSnapshot& snap);  // the constants trace, defined below
}  // namespace

std::string g_plume_frame_summary;


namespace {

// --- Minimal, dependency-free PNG writer used only for one-off texture
// dumps while diagnosing guest texture corruption (XERENGE_DUMP_TEXTURES=1).
// Writes RGBA8 pixels as uncompressed (stored) deflate blocks - valid PNG,
// just not small - so no zlib/libpng linkage is needed for a debug tool.
uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void PutBe32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(uint8_t(v >> 24));
  out->push_back(uint8_t(v >> 16));
  out->push_back(uint8_t(v >> 8));
  out->push_back(uint8_t(v));
}

void WriteChunk(std::vector<uint8_t>* out, const char tag[4], const std::vector<uint8_t>& body) {
  PutBe32(out, uint32_t(body.size()));
  const size_t tag_off = out->size();
  out->insert(out->end(), tag, tag + 4);
  out->insert(out->end(), body.begin(), body.end());
  const uint32_t crc = Crc32(out->data() + tag_off, 4 + body.size());
  PutBe32(out, crc);
}

// Dumps a top-left-origin RGBA8 image (row_bytes may exceed width*4 due to
// upload alignment - only the first width*4 bytes of each row are used).
void DumpTextureToPng(const std::string& path, const uint8_t* pixels, uint32_t width,
                     uint32_t height, uint32_t row_bytes) {
  if (!pixels || width == 0 || height == 0) {
    return;
  }
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (1 + width * 4));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);  // filter type 0 (none) for this scanline
    const uint8_t* row = pixels + size_t(y) * row_bytes;
    raw.insert(raw.end(), row, row + size_t(width) * 4);
  }
  // zlib stream: 2-byte header, one or more stored deflate blocks, 4-byte
  // Adler-32 of the uncompressed data.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t off = 0;
  while (off < raw.size() || (off == 0 && raw.empty())) {
    const size_t chunk = std::min<size_t>(raw.size() - off, 65535);
    const bool last = (off + chunk) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(chunk));
    z.push_back(uint8_t(chunk >> 8));
    z.push_back(uint8_t(~chunk));
    z.push_back(uint8_t(~chunk >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + chunk);
    off += chunk;
    if (raw.empty()) break;
  }
  uint32_t a1 = 1, a2 = 0;
  for (uint8_t byte : raw) {
    a1 = (a1 + byte) % 65521u;
    a2 = (a2 + a1) % 65521u;
  }
  PutBe32(&z, (a2 << 16) | a1);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  PutBe32(&ihdr, width);
  PutBe32(&ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // color type: RGBA
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  WriteChunk(&png, "IHDR", ihdr);
  WriteChunk(&png, "IDAT", z);
  WriteChunk(&png, "IEND", {});

  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
  }
}

// --- Minimal BC1/BC2/BC3 block decoders, used only to make DXT-compressed
// guest textures visible in the debug PNG dump above (the GPU decodes them
// natively at sample time - this exists purely for offline diagnosis of the
// bytes that were actually untiled off guest memory).
void UnpackRgb565(uint16_t c, uint8_t rgb[3]) {
  rgb[0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
  rgb[2] = uint8_t((c & 0x1F) * 255 / 31);
}

// Decodes the 8-byte DXT1 colour part shared by BC1/BC2/BC3. explicit_alpha
// selects the "always 4 opaque colours" interpretation BC2/BC3 use (as
// opposed to BC1's own 1-bit-alpha mode when c0 <= c1).
void DecodeBc1ColorBlock(const uint8_t block[8], bool explicit_alpha, uint8_t out_rgba[16 * 4]) {
  const uint16_t c0 = uint16_t(block[0] | (block[1] << 8));
  const uint16_t c1 = uint16_t(block[2] | (block[3] << 8));
  uint8_t col[4][3];
  UnpackRgb565(c0, col[0]);
  UnpackRgb565(c1, col[1]);
  const bool four_color = explicit_alpha || c0 > c1;
  uint8_t alpha3 = 255;
  if (four_color) {
    for (int i = 0; i < 3; ++i) {
      col[2][i] = uint8_t((2 * col[0][i] + col[1][i]) / 3);
      col[3][i] = uint8_t((col[0][i] + 2 * col[1][i]) / 3);
    }
  } else {
    for (int i = 0; i < 3; ++i) {
      col[2][i] = uint8_t((col[0][i] + col[1][i]) / 2);
      col[3][i] = 0;
    }
    alpha3 = 0;
  }
  const uint32_t indices =
      uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) |
      (uint32_t(block[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    const uint32_t idx = (indices >> (2 * i)) & 3u;
    out_rgba[i * 4 + 0] = col[idx][0];
    out_rgba[i * 4 + 1] = col[idx][1];
    out_rgba[i * 4 + 2] = col[idx][2];
    out_rgba[i * 4 + 3] = (idx == 3) ? alpha3 : 255;
  }
}

void DecodeBc2AlphaBlock(const uint8_t block[8], uint8_t out_rgba[16 * 4]) {
  for (int i = 0; i < 16; ++i) {
    const uint8_t nibble = (block[i / 2] >> ((i % 2) * 4)) & 0xFu;
    out_rgba[i * 4 + 3] = uint8_t(nibble * 17);
  }
}

void DecodeBc3AlphaBlock(const uint8_t block[8], uint8_t out_rgba[16 * 4]) {
  const uint8_t a0 = block[0];
  const uint8_t a1 = block[1];
  uint8_t alpha[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i <= 6; ++i) {
      alpha[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
    }
  } else {
    for (int i = 1; i <= 4; ++i) {
      alpha[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    }
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) {
    bits |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    const uint32_t idx = uint32_t(bits >> (3 * i)) & 7u;
    out_rgba[i * 4 + 3] = alpha[idx];
  }
}

// Decodes a whole plane of 4x4-blocked BC1/BC2/BC3 data (as laid out by
// UntileGuestTexture: block_h rows of row_bytes, bpb bytes per 4x4 block)
// into a top-left-origin RGBA8 image, width*height with row stride width*4.
bool DecodeCompressedForDump(rex::graphics::xenos::TextureFormat base_fmt,
                             const std::vector<uint8_t>& blocks, uint32_t block_row_bytes,
                             uint32_t width, uint32_t height, std::vector<uint8_t>* out_rgba) {
  using rex::graphics::xenos::TextureFormat;
  int bpb = 0;
  bool has_bc2_alpha = false;
  bool has_bc3_alpha = false;
  switch (base_fmt) {
    case TextureFormat::k_DXT1:
      bpb = 8;
      break;
    case TextureFormat::k_DXT2_3:
      bpb = 16;
      has_bc2_alpha = true;
      break;
    case TextureFormat::k_DXT4_5:
      bpb = 16;
      has_bc3_alpha = true;
      break;
    default:
      return false;
  }
  const uint32_t block_w = (width + 3) / 4;
  const uint32_t block_h = (height + 3) / 4;
  const uint32_t dst_row = width * 4;
  out_rgba->assign(size_t(dst_row) * height, 0);
  for (uint32_t by = 0; by < block_h; ++by) {
    for (uint32_t bx = 0; bx < block_w; ++bx) {
      const size_t off = size_t(by) * block_row_bytes + size_t(bx) * bpb;
      if (off + size_t(bpb) > blocks.size()) {
        continue;
      }
      const uint8_t* block = blocks.data() + off;
      uint8_t texel[16 * 4];
      if (has_bc2_alpha) {
        DecodeBc1ColorBlock(block + 8, /*explicit_alpha=*/true, texel);
        DecodeBc2AlphaBlock(block, texel);
      } else if (has_bc3_alpha) {
        DecodeBc1ColorBlock(block + 8, /*explicit_alpha=*/true, texel);
        DecodeBc3AlphaBlock(block, texel);
      } else {
        DecodeBc1ColorBlock(block, /*explicit_alpha=*/false, texel);
      }
      for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint32_t py = by * 4 + ty;
        if (py >= height) {
          break;
        }
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t px = bx * 4 + tx;
          if (px >= width) {
            continue;
          }
          uint8_t* dst = out_rgba->data() + size_t(py) * dst_row + size_t(px) * 4;
          const uint8_t* s = texel + (ty * 4 + tx) * 4;
          dst[0] = s[0];
          dst[1] = s[1];
          dst[2] = s[2];
          dst[3] = s[3];
        }
      }
    }
  }
  return true;
}

// BC1/BC2/BC3 blocks re-encoded as ETC2, for GPUs that take ETC2 and not BC
// (Mali): RGBA8 instead cost eight times the memory and bandwidth of the
// 4-bit formats and made the Pixel's frames slow enough to show half-drawn
// ones. Each block is decoded into a BGRA image padded to whole blocks (the
// order etcpak reads), which etcpak encodes - ETC2 RGB for opaque textures,
// ETC2 RGBA (EAC alpha) otherwise. *alpha comes in as the format chosen for the
// texture's base level, or as -1 to choose it here: from the format, or for
// BC1 from whether any texel is transparent. row_texels is the padded width.
bool TranscodeBcToEtc2(rex::graphics::xenos::TextureFormat base_fmt,
                       const std::vector<uint8_t>& blocks, uint32_t block_row_bytes,
                       uint32_t width, uint32_t height, int* alpha, std::vector<uint8_t>* out,
                       uint32_t* row_texels) {
  using rex::graphics::xenos::TextureFormat;
  int bpb = 0;
  switch (base_fmt) {
    case TextureFormat::k_DXT1:
      bpb = 8;
      break;
    case TextureFormat::k_DXT2_3:
    case TextureFormat::k_DXT4_5:
      bpb = 16;
      break;
    default:
      return false;
  }
  const uint32_t block_w = (width + 3) / 4;
  const uint32_t block_h = (height + 3) / 4;
  const uint32_t padded_w = block_w * 4;
  std::vector<uint32_t> bgra(size_t(padded_w) * block_h * 4, 0xFF000000u);
  std::atomic<bool> transparent{false};
  const auto decode_rows = [&](uint32_t by_begin, uint32_t by_end) {
    bool seen_transparent = false;
    for (uint32_t by = by_begin; by < by_end; ++by) {
      for (uint32_t bx = 0; bx < block_w; ++bx) {
        const size_t off = size_t(by) * block_row_bytes + size_t(bx) * bpb;
        if (off + size_t(bpb) > blocks.size()) {
          continue;
        }
        const uint8_t* block = blocks.data() + off;
        uint8_t texel[16 * 4];
        if (base_fmt == TextureFormat::k_DXT2_3) {
          DecodeBc1ColorBlock(block + 8, /*explicit_alpha=*/true, texel);
          DecodeBc2AlphaBlock(block, texel);
        } else if (base_fmt == TextureFormat::k_DXT4_5) {
          DecodeBc1ColorBlock(block + 8, /*explicit_alpha=*/true, texel);
          DecodeBc3AlphaBlock(block, texel);
        } else {
          DecodeBc1ColorBlock(block, /*explicit_alpha=*/false, texel);
        }
        for (uint32_t ty = 0; ty < 4; ++ty) {
          uint32_t* row = bgra.data() + size_t(by * 4 + ty) * padded_w + bx * 4;
          for (uint32_t tx = 0; tx < 4; ++tx) {
            const uint8_t* s = texel + (ty * 4 + tx) * 4;
            row[tx] = uint32_t(s[2]) | (uint32_t(s[1]) << 8) | (uint32_t(s[0]) << 16) |
                      (uint32_t(s[3]) << 24);
            seen_transparent |= s[3] != 0xFF;
          }
        }
      }
    }
    if (seen_transparent) {
      transparent.store(true, std::memory_order_relaxed);
    }
  };
  // Bands of block rows on the worker threads once a texture is big enough
  // for it to pay (256x256 and up).
  constexpr uint32_t kBandRows = 16;
  const uint32_t bands = (block_h + kBandRows - 1) / kBandRows;
  const auto run_bands = [&](const std::function<void(uint32_t, uint32_t)>& band) {
    if (bands <= 1 || block_w * block_h < 4096) {
      band(0, block_h);
      return;
    }
    WorkerPool::Get().ParallelFor(bands, [&](size_t i) {
      const uint32_t begin = uint32_t(i) * kBandRows;
      band(begin, std::min(block_h, begin + kBandRows));
    });
  };
  run_bands(decode_rows);
  if (*alpha < 0) {
    *alpha = base_fmt != TextureFormat::k_DXT1 || transparent.load() ? 1 : 0;
  }
  const uint32_t block_bytes = *alpha ? 16u : 8u;
  out->assign(size_t(block_w) * block_h * block_bytes, 0);
  run_bands([&](uint32_t by_begin, uint32_t by_end) {
    const uint32_t* src = bgra.data() + size_t(by_begin) * 4 * padded_w;
    auto* dst = reinterpret_cast<uint64_t*>(out->data() + size_t(by_begin) * block_w * block_bytes);
    const uint32_t count = (by_end - by_begin) * block_w;
    if (*alpha) {
      CompressEtc2Rgba(src, dst, count, padded_w, true);
    } else {
      CompressEtc2Rgb(src, dst, count, padded_w, true);
    }
  });
  *row_texels = padded_w;
  return true;
}

constexpr uint32_t kBindlessTextureCount = 4096;
constexpr uint32_t kIdentityLutSize = 64;
constexpr uint32_t kBindlessSamplerCount = 64;
// The sampler slots a draw has: Xenos's sixteen texture fetch slots.
constexpr uint32_t kDrawSlots = 16;
constexpr uint32_t kVsConstantBytes = 256 * 16;
constexpr uint32_t kPsConstantBytes = 256 * 16;
constexpr uint32_t kSharedConstantBytes = 512;
// A single run of UI text reaches ~1000 vertices (each glyph is its own quad,
// and the outline is drawn four times before the fill), so a screenful of
// menu text runs into several thousand. At 4096 the tail of the frame was
// silently dropped, which showed up as truncated strings and panels that
// rendered empty. Every vertex carries all 32 input locations, so this is
// 512 bytes each.
//
// Scene geometry dwarfs any menu: car select filled 32768 outright and dropped
// draws for the rest of the frame. Vertices, not draw slots, are what run out -
// 278 of 2048 slots were in use when the buffer was full - because every vertex
// reserves room for every input location.
//
// The translated shaders declare no location above 19, so 12 of the 32 were
// pure padding: 192 bytes of every 512. Dropping them buys back more than the
// next doubling of the count would have, at no cost in what can be expressed.
// Scene frames fill this outright and then drop everything issued afterwards -
// and the interface is issued last, which is why it disappears the moment a car
// is on screen rather than because anything is wrong with it.
//
// The lasting fix is to pack each vertex to the stride its own shader declares
// instead of a layout wide enough for every shader at once; most draws use four
// or five locations of the twenty. Until then this has to be wide enough to
// hold a whole frame, since a partial frame loses whatever came last.
// Sized for a frame that draws the scene seven times: six faces of the car's
// reflection cube and the scene itself. At 524288 the buffer ran out before
// post-processing, and everything after - the final composite and the whole
// interface - was dropped, over a hundred thousand draws a run.
// And Crash mode's junctions, every car and piece of debris on screen with the
// six faces, filled half of that a frame (524288) and dropped the post-processing
// and the interface again: a black 320x180 corner and no HUD, flickering. Twice
// that on a desktop (640 MB of upload heap); a phone keeps the smaller one.
#if defined(__ANDROID__)
constexpr uint32_t kDummyVertexCount = 1048576;
#else
constexpr uint32_t kDummyVertexCount = 2097152;
#endif
constexpr uint32_t kVertsPerFrame = kDummyVertexCount / 2;
constexpr uint32_t kCacheVertexCount = 1048576;
constexpr uint32_t kInputLocationCount = 20;
constexpr uint32_t kVertexStrideBytes = kInputLocationCount * 16;
// Every location: the layout the staging buffer, the passthrough pipelines and
// the video pipeline use.
constexpr uint32_t kFullLayoutMask = (1u << kInputLocationCount) - 1;
// What the GPU is handed, though, is only what the draw's vertex shader reads.
// The full layout reserves all twenty locations for every vertex while a draw
// uses four or five of them, so a race frame - some 240 thousand vertices -
// wrote 75 MB into memory the GPU then read back, most of it padding. On a
// laptop whose processor and graphics share one memory, both sides pay for
// it. Each vertex still has a full layout's room in the buffer, so nothing
// about where a draw goes changes; only the front of that room is written,
// packed.

// The vertex layout the title's own pipeline for this vertex shader reads (see
// kFullLayoutMask). XERENGE_FULL_VERTICES=1 keeps the full layout everywhere.
uint32_t TitleVertexLayout(uint64_t vs_hash) {
  static const bool packed = std::getenv("XERENGE_FULL_VERTICES") == nullptr;
  return packed ? (PlumeShaderCache::Instance().InputLocationMask(vs_hash) & kFullLayoutMask)
                : kFullLayoutMask;
}

// One slot per encoded draw. Kept in step with the overlay ring that feeds
// this, so a frame the ring managed to hold is not then truncated here.
constexpr uint32_t kCbSlots = 16384;
// Each frame uses half of the vertex and constant buffers, alternating, so the
// frame being written never shares a half with the one the GPU is drawing.
constexpr uint32_t kSlotsPerFrame = kCbSlots / 2;
constexpr uint32_t kCbAlign = 256;
constexpr uint32_t kVsSlotBytes = (kVsConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);
constexpr uint32_t kPsSlotBytes = (kPsConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);
constexpr uint32_t kSharedSlotBytes = (kSharedConstantBytes + kCbAlign - 1) & ~(kCbAlign - 1);

bool Float4Dead(const float* v) {
  return std::fabs(v[0]) < 1e-8f && std::fabs(v[1]) < 1e-8f && std::fabs(v[2]) < 1e-8f &&
         std::fabs(v[3]) < 1e-8f;
}

rex::graphics::xenos::xe_gpu_vertex_fetch_t FetchAt(const GuestDrawSnapshot& snap, uint32_t index);
rex::graphics::xenos::xe_gpu_texture_fetch_t TextureFetchAt(const GuestDrawSnapshot& snap,
                                                            uint32_t slot);
uint64_t TextureKey(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);

bool LooksLikeRgbaColor(const float* v) {
  if (!v) {
    return false;
  }
  int live = 0;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(v[i]) || v[i] < -0.05f || v[i] > 1.05f) {
      return false;
    }
    if (std::fabs(v[i]) > 0.02f) {
      ++live;
    }
  }
  // Reject matrix basis rows like (1,0,0,0) and W-row (0,0,0,1).
  return live >= 2;
}

bool TintNearWhite(const float* t) {
  if (!t) {
    return false;
  }
  const float lo = std::min(std::min(t[0], t[1]), t[2]);
  const float hi = std::max(std::max(t[0], t[1]), t[2]);
  return lo > 0.85f && (hi - lo) < 0.12f;
}

bool TintChromatic(const float* t) {
  if (!t) {
    return false;
  }
  const float chroma =
      std::max(std::fabs(t[0] - t[1]), std::max(std::fabs(t[1] - t[2]), std::fabs(t[2] - t[0])));
  return chroma > 0.12f;
}

// Color UI (flags) is DXT1/DXT3. Title glyphs are 8888 / 8-bit / DXT5.
bool FontLikeUiFormat(rex::graphics::xenos::TextureFormat fmt) {
  using rex::graphics::xenos::TextureFormat;
  return fmt == TextureFormat::k_8_8_8_8 || fmt == TextureFormat::k_8_8_8_8_A ||
         fmt == TextureFormat::k_8 || fmt == TextureFormat::k_8_A ||
         fmt == TextureFormat::k_DXT4_5 || fmt == TextureFormat::k_DXT3A ||
         fmt == TextureFormat::k_DXT5A;
}

bool HasColorAttr(const std::vector<VfetchAttr>& attrs) {
  for (const VfetchAttr& attr : attrs) {
    if (attr.location == 17) {
      return true;
    }
  }
  return false;
}

// Same range check as LooksLikeRgbaColor, without the "at least two live
// components" rule. That rule exists only to keep the *search* from latching
// onto matrix basis rows; once the slot is known there is nothing to guess,
// and demanding two live components would throw away a legitimate black.
bool PlausibleColorValue(const float* v) {
  if (!v) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(v[i]) || v[i] < -0.05f || v[i] > 1.05f) {
      return false;
    }
  }
  return true;
}

bool ResolveUiTint(const GuestDrawSnapshot& snap, bool has_color_attr, float tint[4], int* slot_out,
                   int learned_slot = -1) {
  if (!tint || has_color_attr || snap.has_video_frame() || IsGuestVideoBlit(snap)) {
    return false;
  }
  const float* ps0 = reinterpret_cast<const float*>(snap.ps_constants.data());
  const float* c0 = reinterpret_cast<const float*>(snap.vs_constants.data());
  // A slot already established for this shader wins over re-guessing: it is
  // the only way a black drop-shadow pass can be told apart from "no colour
  // constant here at all".
  if (learned_slot >= 0 && learned_slot < 16) {
    const float* cand = c0 + learned_slot * 4;
    if (PlausibleColorValue(cand)) {
      std::memcpy(tint, cand, 16);
      if (tint[3] <= 1e-6f) {
        tint[3] = 1.0f;
      }
      if (slot_out) {
        *slot_out = learned_slot;
      }
      return true;
    }
  }
  int slot = -1;
  if (LooksLikeRgbaColor(ps0)) {
    std::memcpy(tint, ps0, 16);
    slot = -2;
  } else {
    bool found = false;
    for (uint32_t ci = 0; ci < 16; ++ci) {
      const float* cand = c0 + ci * 4;
      if (LooksLikeRgbaColor(cand)) {
        std::memcpy(tint, cand, 16);
        slot = int(ci);
        found = true;
      }
    }
    if (!found) {
      return false;
    }
  }
  if (tint[3] <= 1e-6f) {
    tint[3] = 1.0f;
  }
  if (slot_out) {
    *slot_out = slot;
  }
  return true;
}

// RB_BLENDCONTROL with the standard "src alpha over dst" setup, used as the
// stand-in when a draw arrives before the guest has programmed the register.
// Field positions match reg::RB_BLENDCONTROL.
constexpr uint32_t kDefaultBlendControl = (6u << 0) |   // color_srcblend  = kSrcAlpha
                                          (0u << 5) |   // color_comb_fcn  = kAdd
                                          (7u << 8) |   // color_destblend = kOneMinusSrcAlpha
                                          (1u << 16) |  // alpha_srcblend  = kOne
                                          (0u << 21) |  // alpha_comb_fcn  = kAdd
                                          (7u << 24);   // alpha_destblend = kOneMinusSrcAlpha

plume::RenderBlend MapBlendFactor(rex::graphics::xenos::BlendFactor factor) {
  using rex::graphics::xenos::BlendFactor;
  switch (factor) {
    case BlendFactor::kZero:
      return plume::RenderBlend::ZERO;
    case BlendFactor::kOne:
      return plume::RenderBlend::ONE;
    case BlendFactor::kSrcColor:
      return plume::RenderBlend::SRC_COLOR;
    case BlendFactor::kOneMinusSrcColor:
      return plume::RenderBlend::INV_SRC_COLOR;
    case BlendFactor::kSrcAlpha:
      return plume::RenderBlend::SRC_ALPHA;
    case BlendFactor::kOneMinusSrcAlpha:
      return plume::RenderBlend::INV_SRC_ALPHA;
    case BlendFactor::kDstColor:
      return plume::RenderBlend::DEST_COLOR;
    case BlendFactor::kOneMinusDstColor:
      return plume::RenderBlend::INV_DEST_COLOR;
    case BlendFactor::kDstAlpha:
      return plume::RenderBlend::DEST_ALPHA;
    case BlendFactor::kOneMinusDstAlpha:
      return plume::RenderBlend::INV_DEST_ALPHA;
    // The Xenos constant-colour factors map onto the single blend constant
    // plume exposes; the UI paths in this title do not use them, so the
    // colour/alpha distinction being flattened here is not observable.
    case BlendFactor::kConstantColor:
    case BlendFactor::kConstantAlpha:
      return plume::RenderBlend::BLEND_FACTOR;
    case BlendFactor::kOneMinusConstantColor:
    case BlendFactor::kOneMinusConstantAlpha:
      return plume::RenderBlend::INV_BLEND_FACTOR;
    case BlendFactor::kSrcAlphaSaturate:
      return plume::RenderBlend::SRC_ALPHA_SAT;
    default:
      return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation MapBlendOp(rex::graphics::xenos::BlendOp op) {
  using rex::graphics::xenos::BlendOp;
  switch (op) {
    case BlendOp::kAdd:
      return plume::RenderBlendOperation::ADD;
    case BlendOp::kSubtract:
      return plume::RenderBlendOperation::SUBTRACT;
    case BlendOp::kMin:
      return plume::RenderBlendOperation::MIN;
    case BlendOp::kMax:
      return plume::RenderBlendOperation::MAX;
    case BlendOp::kRevSubtract:
      return plume::RenderBlendOperation::REV_SUBTRACT;
    default:
      return plume::RenderBlendOperation::ADD;
  }
}

// RB_COLOR_MASK packs a 4-bit write mask per render target; the passthrough
// path only ever renders to target 0.
uint8_t WriteMaskForRt0(uint32_t color_mask) {
  const uint32_t rt0 = color_mask & 0xFu;
  uint8_t mask = 0;
  if (rt0 & 0x1u) mask |= uint8_t(plume::RenderColorWriteEnable::RED);
  if (rt0 & 0x2u) mask |= uint8_t(plume::RenderColorWriteEnable::GREEN);
  if (rt0 & 0x4u) mask |= uint8_t(plume::RenderColorWriteEnable::BLUE);
  if (rt0 & 0x8u) mask |= uint8_t(plume::RenderColorWriteEnable::ALPHA);
  return mask;
}

bool GuestBlendIsOpaqueCopy(uint32_t blend_control);

// Rebuilds the guest's own blend state rather than forcing one fixed mode.
plume::RenderBlendDesc BlendDescFromGuest(uint32_t blend_control, uint8_t write_mask) {
  rex::graphics::reg::RB_BLENDCONTROL bc;
  bc.value = blend_control;

  plume::RenderBlendDesc desc;
  desc.srcBlend = MapBlendFactor(bc.color_srcblend);
  desc.dstBlend = MapBlendFactor(bc.color_destblend);
  desc.blendOp = MapBlendOp(bc.color_comb_fcn);
  desc.srcBlendAlpha = MapBlendFactor(bc.alpha_srcblend);
  desc.dstBlendAlpha = MapBlendFactor(bc.alpha_destblend);
  desc.blendOpAlpha = MapBlendOp(bc.alpha_comb_fcn);
  desc.renderTargetWriteMask = write_mask;
  // ONE/ZERO/ADD on both channels is a plain overwrite - leaving blending
  // switched on for it would cost nothing but says the wrong thing, and some
  // drivers take a slower path for it.
  const bool is_opaque_copy = desc.srcBlend == plume::RenderBlend::ONE &&
                              desc.dstBlend == plume::RenderBlend::ZERO &&
                              desc.blendOp == plume::RenderBlendOperation::ADD &&
                              desc.srcBlendAlpha == plume::RenderBlend::ONE &&
                              desc.dstBlendAlpha == plume::RenderBlend::ZERO &&
                              desc.blendOpAlpha == plume::RenderBlendOperation::ADD;
  desc.blendEnabled = !is_opaque_copy;
  return desc;
}

// True when the draw would simply overwrite the target (ONE/ZERO/ADD), i.e.
// there is nothing for the framebuffer underneath to show through.
bool GuestBlendIsOpaqueCopy(uint32_t blend_control) {
  return !BlendDescFromGuest(blend_control, 0xFu).blendEnabled;
}

plume::RenderComparisonFunction MapCompareFunction(rex::graphics::xenos::CompareFunction func) {
  using rex::graphics::xenos::CompareFunction;
  switch (func) {
    case CompareFunction::kNever:
      return plume::RenderComparisonFunction::NEVER;
    case CompareFunction::kLess:
      return plume::RenderComparisonFunction::LESS;
    case CompareFunction::kEqual:
      return plume::RenderComparisonFunction::EQUAL;
    case CompareFunction::kLessEqual:
      return plume::RenderComparisonFunction::LESS_EQUAL;
    case CompareFunction::kGreater:
      return plume::RenderComparisonFunction::GREATER;
    case CompareFunction::kNotEqual:
      return plume::RenderComparisonFunction::NOT_EQUAL;
    case CompareFunction::kGreaterEqual:
      return plume::RenderComparisonFunction::GREATER_EQUAL;
    case CompareFunction::kAlways:
    default:
      return plume::RenderComparisonFunction::ALWAYS;
  }
}

// Depth test and write exactly as the guest programmed them. UI runs with the
// test off, so screen-space drawing is unaffected by honouring this; 3D
// geometry is not, which is the whole point.
struct GuestDepthState {
  bool enabled = false;
  bool write = false;
  plume::RenderComparisonFunction function = plume::RenderComparisonFunction::ALWAYS;
};

GuestDepthState DepthStateFromGuest(uint32_t depth_control) {
  rex::graphics::reg::RB_DEPTHCONTROL dc;
  dc.value = depth_control;
  GuestDepthState state;
  state.enabled = dc.z_enable != 0;
  state.write = dc.z_write_enable != 0;
  state.function = MapCompareFunction(dc.zfunc);
  return state;
}

bool VulkanPipelineOk(const plume::RenderPipeline* pipeline) {
  const auto* vk_pipeline = static_cast<const plume::VulkanGraphicsPipeline*>(pipeline);
  return vk_pipeline && vk_pipeline->vk != VK_NULL_HANDLE;
}

bool VulkanBufferOk(const plume::RenderBuffer* buffer) {
  const auto* vk_buffer = static_cast<const plume::VulkanBuffer*>(buffer);
  return vk_buffer && vk_buffer->vk != VK_NULL_HANDLE;
}

// plume hands back a texture object even when the image could not be made (an
// unsupported format, say): only its handle tells.
bool VulkanTextureOk(const plume::RenderTexture* texture) {
  const auto* vk_texture = static_cast<const plume::VulkanTexture*>(texture);
  return vk_texture && vk_texture->vk != VK_NULL_HANDLE;
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
            attr.instr_address = address + i;
            attr.stride_dwords = last_stride;
            attr.offset_dwords = vfetch.offset();
            attr.format = uint32_t(vfetch.data_format());
            attr.exp_adjust = vfetch.exp_adjust();
            attr.is_signed = vfetch.is_signed();
            attr.is_normalized = vfetch.is_normalized();
            attr.src_reg = vfetch.src();
            attr.src_comp = vfetch.src_swizzle() & 3u;
            attr.dst_reg = vfetch.dest();
            attr.dst_swiz = vfetch.dest_swizzle();
            attr.index_rounded = vfetch.is_index_rounded();
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
            // Kept even past the last input: the title's own shaders get their
            // real locations later, by position in this list, and dropping the
            // fourth colour-like attribute here left the wreck's damage
            // shader one input short - and zero. FillVertices skips whatever
            // still has no room.
            attrs.push_back(attr);
          }
        }
        sequence >>= 2;
      }
    }
    cf_off += 12;
  }
  return attrs;
}

// Interpolator registers a vertex shader exports to, ascending and unique.
// Walks the same control flow as ParseVfetches, but looks at the ALU slots:
// an exporting ALU instruction names its destination, and ExportRegister puts
// the interpolators at 0-15 (62 is position, 63 point size - neither is an
// interpolator). This is what tells a container wrapper how many varyings the
// shader actually produces.
std::vector<uint32_t> ParseVsExportRegisters(const uint8_t* bytes, uint32_t byte_size) {
  std::vector<uint32_t> registers;
  if (!bytes || byte_size < 12) {
    return registers;
  }

  using rex::graphics::ucode::AluInstruction;
  using rex::graphics::ucode::ControlFlowInstruction;
  using rex::graphics::ucode::ControlFlowOpcode;
  using rex::graphics::ucode::UnpackControlFlowInstructions;

  auto add = [&](uint32_t reg) {
    if (reg >= 16) {
      return;
    }
    if (std::find(registers.begin(), registers.end(), reg) == registers.end()) {
      registers.push_back(reg);
    }
  };

  uint32_t cf_off = 0;
  while (cf_off + 12 <= byte_size) {
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
        // Bit 0 of each pair selects fetch over ALU; only ALU slots export.
        if ((sequence & 1u) == 0) {
          const uint32_t raw[3] = {LoadBeU32(code + i * 12), LoadBeU32(code + i * 12 + 4),
                                   LoadBeU32(code + i * 12 + 8)};
          AluInstruction alu{};
          std::memcpy(&alu, raw, sizeof(raw));
          if (alu.is_export()) {
            add(alu.vector_dest());
            add(alu.scalar_dest());
          }
        }
        sequence >>= 2;
      }
    }
    cf_off += 12;
  }
  std::sort(registers.begin(), registers.end());
  return registers;
}

// Sampler registers a shader fetches textures from. Unlike a vertex fetch,
// whose constant index is const_index * 3 + const_index_sel, a texture fetch
// names its sampler directly - which is the number XenosRecomp falls back to
// when it prints s<N>, so a container has to declare exactly these.
std::vector<uint32_t> ParseSamplerRegisters(const uint8_t* bytes, uint32_t byte_size) {
  std::vector<uint32_t> samplers;
  if (!bytes || byte_size < 12) {
    return samplers;
  }

  using rex::graphics::ucode::ControlFlowInstruction;
  using rex::graphics::ucode::ControlFlowOpcode;
  using rex::graphics::ucode::FetchOpcode;
  using rex::graphics::ucode::TextureFetchInstruction;
  using rex::graphics::ucode::UnpackControlFlowInstructions;

  uint32_t cf_off = 0;
  while (cf_off + 12 <= byte_size) {
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
          TextureFetchInstruction tfetch{};
          std::memcpy(&tfetch, raw, sizeof(raw));
          if (tfetch.opcode() != FetchOpcode::kVertexFetch) {
            const uint32_t reg = tfetch.fetch_constant_index();
            if (reg < 32 && std::find(samplers.begin(), samplers.end(), reg) == samplers.end()) {
              samplers.push_back(reg);
            }
          }
        }
        sequence >>= 2;
      }
    }
    cf_off += 12;
  }
  std::sort(samplers.begin(), samplers.end());
  return samplers;
}

// Interpolator registers a pixel shader consumes. The rasteriser hands the
// interpolated values to the pixel shader in temporary registers before the
// program starts, so any temporary it reads before it has written is an input
// coming from the vertex shader. Deriving these is what lets a wrapped pixel
// shader declare varyings at all: the existing wrapper declares none, which
// leaves such a shader receiving nothing from the vertex stage.
std::vector<uint32_t> ParsePsInterpolatorRegisters(const uint8_t* bytes, uint32_t byte_size) {
  std::vector<uint32_t> inputs;
  if (!bytes || byte_size < 12) {
    return inputs;
  }

  using rex::graphics::ucode::AluInstruction;
  using rex::graphics::ucode::ControlFlowInstruction;
  using rex::graphics::ucode::ControlFlowOpcode;
  using rex::graphics::ucode::UnpackControlFlowInstructions;

  uint64_t written = 0;
  auto note_read = [&](uint32_t reg) {
    if (reg >= 64 || (written & (uint64_t(1) << reg)) != 0) {
      return;
    }
    if (std::find(inputs.begin(), inputs.end(), reg) == inputs.end()) {
      inputs.push_back(reg);
    }
  };

  uint32_t cf_off = 0;
  while (cf_off + 12 <= byte_size) {
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
        if ((sequence & 1u) == 0) {
          const uint32_t raw[3] = {LoadBeU32(code + i * 12), LoadBeU32(code + i * 12 + 4),
                                   LoadBeU32(code + i * 12 + 8)};
          AluInstruction alu{};
          std::memcpy(&alu, raw, sizeof(raw));
          // Reads are examined before the writes of the same instruction, so a
          // register this instruction both reads and writes still counts as an
          // input.
          for (size_t src = 1; src <= 3; ++src) {
            if (alu.src_is_temp(src)) {
              note_read(AluInstruction::src_temp_reg(alu.src_reg(src)));
            }
          }
          if (!alu.is_export()) {
            if (alu.vector_dest() < 64) {
              written |= uint64_t(1) << alu.vector_dest();
            }
            if (alu.scalar_dest() < 64) {
              written |= uint64_t(1) << alu.scalar_dest();
            }
          }
        }
        sequence >>= 2;
      }
    }
    cf_off += 12;
  }
  std::sort(inputs.begin(), inputs.end());
  return inputs;
}

constexpr uint32_t kFloatsPerVert = kVertexStrideBytes / 4;

// Xenos drops a primitive one of whose vertices has a NaN position; a PC GPU does not have to, and AMD rasterizes it
// as a slab or a needle across the screen. The title's particles make such vertices themselves (a zero-length
// vector normalized with vrsqrtefp), so the console's behaviour is emulated: every primitive touching a vertex with a
// non-finite or absurd position is collapsed to one point, which has no area and draws nothing. The vertices are
// already one per index here, so primitives are consecutive. Returns how many vertices were bad.
bool BadPosition(const float* p) {
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1.0e12f) {
      return true;
    }
  }
  return false;
}

uint32_t CollapseBadPrimitives(rex::graphics::xenos::PrimitiveType prim, float* vb, uint32_t vertex_count,
                               uint32_t pos_float) {
  using rex::graphics::xenos::PrimitiveType;
  auto pos = [&](uint32_t i) { return vb + size_t(i) * kFloatsPerVert + pos_float; };
  uint32_t bad = 0;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    bad += BadPosition(pos(i)) ? 1u : 0u;
  }
  if (bad == 0) {
    return 0;
  }
  static const float kOrigin[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  // A primitive of `size` consecutive vertices goes to the place of its first good vertex.
  auto collapse_groups = [&](uint32_t size) {
    for (uint32_t first = 0; first + size <= vertex_count; first += size) {
      const float* good = nullptr;
      bool any_bad = false;
      for (uint32_t k = 0; k < size; ++k) {
        if (BadPosition(pos(first + k))) {
          any_bad = true;
        } else if (!good) {
          good = pos(first + k);
        }
      }
      if (any_bad) {
        float point[4];
        std::memcpy(point, good ? good : kOrigin, sizeof(point));
        for (uint32_t k = 0; k < size; ++k) {
          std::memcpy(pos(first + k), point, sizeof(point));
        }
      }
    }
  };
  switch (prim) {
    case PrimitiveType::kTriangleList:
    case PrimitiveType::kRectangleList:
      collapse_groups(3);
      break;
    case PrimitiveType::kQuadList:
      collapse_groups(4);
      break;
    case PrimitiveType::kLineList:
      collapse_groups(2);
      break;
    case PrimitiveType::kTriangleFan:
      // Every triangle of a fan has the centre in it.
      if (BadPosition(pos(0))) {
        collapse_groups(vertex_count);
        break;
      }
      [[fallthrough]];
    default: {
      // Strips share vertices between primitives: a bad vertex takes its neighbour's place, which leaves the
      // primitives it belonged to without area. The nearest good vertex before it, else after it.
      for (uint32_t i = 0; i < vertex_count; ++i) {
        if (!BadPosition(pos(i))) {
          continue;
        }
        const float* good = nullptr;
        for (uint32_t j = i; j-- > 0 && !good;) {
          good = BadPosition(pos(j)) ? nullptr : pos(j);
        }
        for (uint32_t j = i + 1; j < vertex_count && !good; ++j) {
          good = BadPosition(pos(j)) ? nullptr : pos(j);
        }
        std::memcpy(pos(i), good ? good : kOrigin, 4 * sizeof(float));
      }
      break;
    }
  }
  return bad;
}

float Dist2XY(const float* a, const float* b) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  return dx * dx + dy * dy;
}

// Xenos rectangle lists are 3 verts; the longest edge is the diagonal.
// Mirror the right-angle vertex across that edge to get the fourth corner.
// A Xenos rectangle list holds one rectangle per three vertices, and a draw
// may carry several. Each becomes two triangles, so the output is twice the
// input - expanding in place would trample the rectangles not yet read, hence
// the copy.
uint32_t ExpandRectangle(float* vb, uint32_t base_vertex, uint32_t vertex_count,
                         uint32_t pos_float = 0) {
  const uint32_t rect_count = vertex_count / 3;
  if (rect_count == 0) {
    return vertex_count;
  }
  uint32_t out_rects = rect_count;
  while (out_rects > 0 && base_vertex + out_rects * 6 > kDummyVertexCount) {
    --out_rects;
  }
  if (out_rects == 0) {
    return 0;
  }

  const float* first = vb + size_t(base_vertex) * kFloatsPerVert;
  std::vector<float> src(first, first + size_t(rect_count) * 3 * kFloatsPerVert);

  for (uint32_t r = 0; r < out_rects; ++r) {
    const float* v0 = src.data() + size_t(r * 3 + 0) * kFloatsPerVert;
    const float* v1 = src.data() + size_t(r * 3 + 1) * kFloatsPerVert;
    const float* v2 = src.data() + size_t(r * 3 + 2) * kFloatsPerVert;
    // The three given corners form half of the rectangle; the longest edge is
    // its diagonal, so mirroring the remaining corner across that diagonal
    // gives the fourth.
    std::vector<float> v3(kFloatsPerVert);
    // The diagonal's two ends: the second triangle is built on it, with the
    // new corner opposite the right angle. Always pairing v0 and v2 with it was
    // right only when the right angle was at v1; with it at v0 - as a
    // full-screen pass gives it - the second triangle covered the first one's
    // half again and a quarter of the screen was never drawn.
    const float* diag_a = v0;
    const float* diag_b = v2;
    const float d01 = Dist2XY(v0 + pos_float, v1 + pos_float);
    const float d02 = Dist2XY(v0 + pos_float, v2 + pos_float);
    const float d12 = Dist2XY(v1 + pos_float, v2 + pos_float);
    if (d01 >= d02 && d01 >= d12) {
      for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
        v3[c] = v0[c] + v1[c] - v2[c];
      }
      diag_a = v0;
      diag_b = v1;
    } else if (d02 >= d12) {
      for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
        v3[c] = v0[c] + v2[c] - v1[c];
      }
    } else {
      for (uint32_t c = 0; c < kFloatsPerVert; ++c) {
        v3[c] = v1[c] + v2[c] - v0[c];
      }
      diag_a = v1;
      diag_b = v2;
    }

    float* dst = vb + size_t(base_vertex + r * 6) * kFloatsPerVert;
    auto put = [&](uint32_t i, const float* s) {
      std::memcpy(dst + size_t(i) * kFloatsPerVert, s, kVertexStrideBytes);
    };
    put(0, v0);
    put(1, v1);
    put(2, v2);
    put(3, diag_a);
    put(4, v3.data());
    put(5, diag_b);

    static uint32_t rect_logs = 0;
    if (rect_logs < 12) {
      ++rect_logs;
      REXLOG_INFO(
          "plume: rect {}/{} v0=({:.3f},{:.3f}) v1=({:.3f},{:.3f}) v2=({:.3f},{:.3f}) "
          "-> v3=({:.3f},{:.3f}) verts_in={}",
          r + 1, rect_count, v0[0], v0[1], v1[0], v1[1], v2[0], v2[1], v3[0], v3[1], vertex_count);
    }
  }
  if (out_rects != rect_count) {
    REXLOG_WARN("plume: rectangle list truncated {} -> {} rects", rect_count, out_rects);
  }
  return out_rects * 6;
}

// A quad list gives all four corners per quad, in perimeter order, so unlike a
// rectangle list nothing has to be reconstructed - it just needs splitting
// into two triangles. Handing the four vertices straight to a triangle list
// instead drew one triangle from the first three and dropped the fourth,
// which is why quads came out as wedges.
uint32_t ExpandQuadList(float* vb, uint32_t base_vertex, uint32_t vertex_count) {
  const uint32_t quad_count = vertex_count / 4;
  if (quad_count == 0) {
    return vertex_count;
  }
  uint32_t out_quads = quad_count;
  while (out_quads > 0 && base_vertex + out_quads * 6 > kDummyVertexCount) {
    --out_quads;
  }
  if (out_quads == 0) {
    return 0;
  }

  // Six output vertices per four input ones, so the tail of the input has to
  // be read before it is overwritten.
  const float* first = vb + size_t(base_vertex) * kFloatsPerVert;
  std::vector<float> src(first, first + size_t(quad_count) * 4 * kFloatsPerVert);

  for (uint32_t q = 0; q < out_quads; ++q) {
    const float* v0 = src.data() + size_t(q * 4 + 0) * kFloatsPerVert;
    const float* v1 = src.data() + size_t(q * 4 + 1) * kFloatsPerVert;
    const float* v2 = src.data() + size_t(q * 4 + 2) * kFloatsPerVert;
    const float* v3 = src.data() + size_t(q * 4 + 3) * kFloatsPerVert;
    float* dst = vb + size_t(base_vertex + q * 6) * kFloatsPerVert;
    auto put = [&](uint32_t i, const float* s) {
      std::memcpy(dst + size_t(i) * kFloatsPerVert, s, kVertexStrideBytes);
    };
    put(0, v0);
    put(1, v1);
    put(2, v2);
    put(3, v0);
    put(4, v2);
    put(5, v3);
  }
  if (out_quads != quad_count) {
    REXLOG_WARN("plume: quad list truncated {} -> {} quads", quad_count, out_quads);
  }
  return out_quads * 6;
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
    // Packed integer formats, laid out as the GPU reads them: components from
    // the low bits up. They used to fall to the default below, which copied
    // the raw word in as a float - so every packed normal and tangent came out
    // as a denormal near zero, and 16-bit texture coordinates as garbage: the
    // car's lighting and the wheel rims' palette lookups with them.
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_16_16: {
      uint32_t bits[4] = {};
      uint32_t count = 0;
      switch (format) {
        case VertexFormat::k_2_10_10_10:
          bits[0] = 10; bits[1] = 10; bits[2] = 10; bits[3] = 2; count = 4;
          break;
        case VertexFormat::k_10_11_11:
          bits[0] = 11; bits[1] = 11; bits[2] = 10; count = 3;
          break;
        case VertexFormat::k_11_11_10:
          bits[0] = 10; bits[1] = 11; bits[2] = 11; count = 3;
          break;
        case VertexFormat::k_16_16:
          bits[0] = 16; bits[1] = 16; count = 2;
          break;
        default:
          bits[0] = bits[1] = bits[2] = bits[3] = 16; count = 4;
          break;
      }
      uint32_t shift = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t n = bits[i];
        const uint32_t word = data[shift / 32];
        const uint32_t raw = (word >> (shift % 32)) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
        shift += n;
        if (is_signed) {
          const int32_t value = int32_t(raw << (32 - n)) >> (32 - n);
          out[i] = is_normalized
                       ? std::max(float(value) / float((1u << (n - 1)) - 1u), -1.0f)
                       : float(value);
        } else {
          out[i] = is_normalized ? float(raw) / float((1u << n) - 1u) : float(raw);
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
  // The mips (their address, whether they are packed, how many) are part of
  // it too: they decide how many levels the host texture gets.
  const uint64_t mips = (uint64_t(fetch.dword_5) >> 11) ^ (uint64_t(fetch.mip_max_level) << 21) ^
                        (uint64_t(fetch.mip_filter) << 25);
  return (uint64_t(fetch.base_address) << 32) ^ (uint64_t(fetch.format) << 16) ^
         uint64_t(fetch.dword_2) ^ (mips << 36);
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
    case TextureFormat::k_8_8_8_8_A:
    case TextureFormat::k_Cr_Y1_Cb_Y0_REP:
    case TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      *host = plume::RenderFormat::R8G8B8A8_UNORM;
      return true;
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
    case TextureFormat::k_8_B:
      *host = plume::RenderFormat::R8G8B8A8_UNORM;
      *expand_r8 = true;
      return true;
    default:
      return false;
  }
}

// Bytes of guest memory a texture is built from - the extent UntileGuestTexture
// reads, so hashing this range is enough to tell whether the source changed.
size_t GuestTextureSourceSize(const rex::graphics::TextureInfo& info) {
  const rex::graphics::FormatInfo* fi = info.format_info();
  if (!fi) {
    return 0;
  }
  const uint32_t bpb = fi->bytes_per_block();
  if (bpb == 0 || fi->block_width == 0 || fi->block_height == 0) {
    return 0;
  }
  const uint32_t width = info.width + 1;
  const uint32_t height = info.height + 1;
  const uint32_t block_w = rex::align(width, fi->block_width) / fi->block_width;
  const uint32_t block_h = rex::align(height, fi->block_height) / fi->block_height;
  if (info.is_tiled) {
    return size_t(rex::align(block_w, 32u)) * rex::align(block_h, 32u) * bpb;
  }
  const uint32_t pitch_blocks = std::max(1u, info.pitch / fi->block_width);
  return size_t(rex::align(pitch_blocks * bpb, 256u)) * block_h;
}

bool UntileGuestTexture(const rex::graphics::TextureInfo& info, const uint8_t* src,
                        std::vector<uint8_t>* out, uint32_t* row_bytes_out,
                        uint32_t* row_texels_out, bool expand_r8, bool alpha_mask = false) {
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
    // In the tiled layout a run of 16 bytes along x - eight blocks of one or
    // two bytes, four of four, two of eight - from a multiple of its length
    // lies in one piece (worked through the address formula for every block
    // size). Copied as one, a block address and a copy call per byte were 20 ms
    // of a 1280x720 video frame on the Pixel. Still checked by the run's last
    // address rather than assumed; and only where swapping the run is the same
    // as swapping each block - no swap, or blocks no smaller than its unit.
    using rex::graphics::xenos::Endian;
    const uint32_t swap_unit = info.endianness == Endian::k8in16 ? 2u
                               : info.endianness == Endian::kNone ? 1u
                                                                   : 4u;
    const uint32_t run_blocks = std::min(8u, std::max(1u, 16u / bpb));
    const bool runs = bpb >= swap_unit && run_blocks > 1;
    for (uint32_t by = 0; by < block_h; ++by) {
      uint8_t* dst_row = blocks.data() + size_t(by) * row_bytes;
      for (uint32_t bx = 0; bx < block_w;) {
        const int32_t gx = pack_x + int32_t(bx);
        const int32_t gy = pack_y + int32_t(by);
        const int32_t so =
            rex::graphics::texture_util::GetTiledOffset2D(gx, gy, aligned_w, bpb_log2);
        uint32_t run = 1;
        if (runs && (uint32_t(gx) % run_blocks) == 0 && bx + run_blocks <= block_w &&
            rex::graphics::texture_util::GetTiledOffset2D(gx + int32_t(run_blocks) - 1, gy,
                                                          aligned_w, bpb_log2) ==
                so + int32_t((run_blocks - 1) * bpb)) {
          run = run_blocks;
        }
        if (run != 1 && (so < 0 || uint32_t(so) + run * bpb > tiled_size)) {
          run = 1;  // at the end of the data: block by block, as before
        }
        if (so >= 0 && uint32_t(so) + run * bpb <= tiled_size) {
          rex::graphics::texture_conversion::CopySwapBlock(
              info.endianness, dst_row + size_t(bx) * bpb, src + so, size_t(run) * bpb);
        }
        bx += run;
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
        if (alpha_mask) {
          p[0] = p[1] = p[2] = 255;
          p[3] = r;
        } else {
          p[0] = p[1] = p[2] = p[3] = r;
        }
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

void SwizzleBgraToRgba(std::vector<uint8_t>* pixels, uint32_t height, uint32_t row_bytes) {
  if (!pixels) {
    return;
  }
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = pixels->data() + size_t(y) * row_bytes;
    for (uint32_t x = 0; x + 3 < row_bytes; x += 4) {
      std::swap(row[x + 0], row[x + 2]);
    }
  }
}

bool ForceOpaqueIfXrgb(std::vector<uint8_t>* pixels, uint32_t width, uint32_t height,
                       uint32_t row_bytes) {
  if (!pixels || width == 0 || height == 0 || row_bytes < 4) {
    return false;
  }
  bool any_alpha = false;
  bool any_color = false;
  for (uint32_t y = 0; y < height && !any_alpha; ++y) {
    const uint8_t* row = pixels->data() + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = row + x * 4;
      if (p[3] != 0) {
        any_alpha = true;
        break;
      }
      if ((p[0] | p[1] | p[2]) != 0) {
        any_color = true;
      }
    }
  }
  if (any_alpha || !any_color) {
    return false;
  }
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = pixels->data() + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 3] = 255;
    }
  }
  return true;
}

// Where a 2D texture's mip levels are in guest memory. The layout is the
// guest's own (texture_util, as Xenia's texture cache loads it): level L sits
// at the mips' address plus mip_offsets_bytes[L] with that level's row pitch,
// except that from packed_level on the small levels share one tile, each at
// its own block offset within it.
struct GuestMipSource {
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 0;  // below the base
  rex::graphics::texture_util::TextureGuestLayout layout;
};

bool LocateGuestMips(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                     GuestMipSource* out) {
  using rex::graphics::xenos::DataDimension;
  using rex::graphics::xenos::TextureFilter;
  if (fetch.dimension != DataDimension::k2DOrStacked || fetch.mip_filter == TextureFilter::kBaseMap) {
    return false;
  }
  uint32_t width_minus_1 = 0, height_minus_1 = 0, depth_minus_1 = 0;
  uint32_t base_page = 0, mip_page = 0, mip_min = 0, mip_max = 0;
  rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page, &mip_page, &mip_min,
      &mip_max);
  if (depth_minus_1 != 0 || base_page == 0 || mip_page == 0 || mip_min != 0 || mip_max == 0) {
    return false;
  }
  const rex::graphics::FormatInfo* fi =
      rex::graphics::FormatInfo::Get(uint32_t(rex::graphics::GetBaseFormat(fetch.format)));
  if (!fi || fi->bytes_per_block() == 0) {
    return false;
  }
  out->width = width_minus_1 + 1;
  out->height = height_minus_1 + 1;
  out->layout = rex::graphics::texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, out->width, out->height, 1, fetch.tiled != 0,
      rex::graphics::GetBaseFormat(fetch.format), fetch.packed_mips != 0, true, mip_max);
  out->levels = out->layout.max_level;
  out->address = mip_page << 12;
  out->size = out->layout.mips_total_extent_bytes;
  return out->levels != 0 && out->size != 0;
}

// What a texture was built from: its base and, when it has them, its mips.
uint64_t TextureContentHash(const uint8_t* src, size_t size, const uint8_t* mip_src,
                            size_t mip_size) {
  uint64_t hash = XXH3_64bits(src, size);
  if (mip_src && mip_size) {
    hash ^= XXH3_64bits(mip_src, mip_size) * 0x9E3779B97F4A7C15ull;
  }
  return hash;
}

// Decodes the mips LocateGuestMips found, into the same host format the base
// was decoded to: expanded from 8 bits, swapped from BGRA, made opaque - each
// only when the base was. Stops at the first level it cannot read.
void DecodeGuestMips(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                     const GuestMipSource& source, const uint8_t* src,
                     rex::graphics::xenos::Endian endian, bool expand_r8, bool alpha_mask,
                     bool swizzle_bgra, bool force_opaque, std::vector<HostMipLevel>* out) {
  const auto format = rex::graphics::GetBaseFormat(fetch.format);
  const rex::graphics::FormatInfo* fi = rex::graphics::FormatInfo::Get(uint32_t(format));
  const uint32_t bpb = fi->bytes_per_block();
  const uint32_t bpb_log2 = rex::log2_floor(bpb);
  const auto& layout = source.layout;
  for (uint32_t level = 1; level <= source.levels; ++level) {
    const uint32_t stored = std::min(level, layout.packed_level);
    if (stored >= rex::graphics::xenos::kTextureMaxMips) {
      break;
    }
    const auto& level_layout = layout.mips[stored];
    const uint32_t level_offset = layout.mip_offsets_bytes[stored];
    uint32_t pack_x = 0, pack_y = 0, pack_z = 0;
    if (level >= layout.packed_level) {
      rex::graphics::texture_util::GetPackedMipOffset(source.width, source.height, 1, format,
                                                      level, pack_x, pack_y, pack_z);
    }
    const uint32_t width = std::max(source.width >> level, 1u);
    const uint32_t height = std::max(source.height >> level, 1u);
    const uint32_t block_w = (width + fi->block_width - 1) / fi->block_width;
    const uint32_t block_h = (height + fi->block_height - 1) / fi->block_height;
    const uint32_t row_bytes = rex::align(block_w * bpb, 256u);
    std::vector<uint8_t> blocks(size_t(row_bytes) * block_h, 0);
    bool inside = level_layout.row_pitch_bytes != 0;
    for (uint32_t by = 0; by < block_h && inside; ++by) {
      for (uint32_t bx = 0; bx < block_w; ++bx) {
        int64_t offset;
        if (fetch.tiled) {
          offset = rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(pack_x + bx), int32_t(pack_y + by), level_layout.row_pitch_bytes >> bpb_log2,
              bpb_log2);
        } else {
          offset = int64_t(pack_y + by) * level_layout.row_pitch_bytes + int64_t(pack_x + bx) * bpb;
        }
        offset += level_offset;
        if (offset < 0 || uint64_t(offset) + bpb > source.size) {
          inside = false;
          break;
        }
        rex::graphics::texture_conversion::CopySwapBlock(
            endian, blocks.data() + size_t(by) * row_bytes + size_t(bx) * bpb, src + offset, bpb);
      }
    }
    if (!inside) {
      break;
    }
    HostMipLevel mip;
    mip.width = width;
    mip.height = height;
    if (expand_r8) {
      const uint32_t dst_row = rex::align(width * 4u, 256u);
      mip.pixels.assign(size_t(dst_row) * height, 0);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t r = blocks[size_t(y) * row_bytes + x];
          uint8_t* p = mip.pixels.data() + size_t(y) * dst_row + size_t(x) * 4;
          if (alpha_mask) {
            p[0] = p[1] = p[2] = 255;
            p[3] = r;
          } else {
            p[0] = p[1] = p[2] = p[3] = r;
          }
        }
      }
      mip.row_texels = dst_row / 4;
    } else {
      if (swizzle_bgra) {
        SwizzleBgraToRgba(&blocks, block_h, row_bytes);
      }
      if (force_opaque) {
        for (uint32_t y = 0; y < height; ++y) {
          uint8_t* row = blocks.data() + size_t(y) * row_bytes;
          for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 3] = 255;
          }
        }
      }
      mip.pixels = std::move(blocks);
      mip.row_texels = (row_bytes / bpb) * fi->block_width;
    }
    out->push_back(std::move(mip));
  }
}

int ClampByte(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void YuvToRgba(int y, int u, int v, uint8_t* dst) {
  y -= 16;
  u -= 128;
  v -= 128;
  dst[0] = uint8_t(ClampByte((298 * y + 409 * v + 128) >> 8));
  dst[1] = uint8_t(ClampByte((298 * y - 100 * u - 208 * v + 128) >> 8));
  dst[2] = uint8_t(ClampByte((298 * y + 516 * u + 128) >> 8));
  dst[3] = 255;
}

bool Convert422ToRgba(rex::graphics::xenos::TextureFormat format, uint32_t width, uint32_t height,
                      std::vector<uint8_t>* pixels, uint32_t* row_bytes, uint32_t* row_texels) {
  if (!pixels || !row_bytes || !row_texels || *row_bytes < 4) {
    return false;
  }
  const uint32_t src_row = *row_bytes;
  const uint32_t dst_row = rex::align(width * 4u, 256u);
  std::vector<uint8_t> rgba(size_t(dst_row) * height, 0);
  const bool cr_y1_cb_y0 = format == rex::graphics::xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = pixels->data() + size_t(y) * src_row;
    uint8_t* dst = rgba.data() + size_t(y) * dst_row;
    for (uint32_t x = 0; x + 1 < width; x += 2) {
      const uint8_t* p = src + (x / 2) * 4;
      int y0, y1, cb, cr;
      if (cr_y1_cb_y0) {
        cr = p[0];
        y1 = p[1];
        cb = p[2];
        y0 = p[3];
      } else {
        y1 = p[0];
        cr = p[1];
        y0 = p[2];
        cb = p[3];
      }
      YuvToRgba(y0, cb, cr, dst + x * 4);
      YuvToRgba(y1, cb, cr, dst + (x + 1) * 4);
    }
  }
  *pixels = std::move(rgba);
  *row_bytes = dst_row;
  *row_texels = dst_row / 4;
  return true;
}

bool IsLumaFormat(rex::graphics::xenos::TextureFormat format) {
  using rex::graphics::xenos::TextureFormat;
  format = rex::graphics::GetBaseFormat(format);
  return format == TextureFormat::k_8 || format == TextureFormat::k_8_A ||
         format == TextureFormat::k_8_B;
}

bool IsPackedUvFormat(rex::graphics::xenos::TextureFormat format) {
  return rex::graphics::GetBaseFormat(format) == rex::graphics::xenos::TextureFormat::k_8_8;
}

// XERENGE_VIDEO_GPU: how much of the video frame's conversion runs on the
// GPU, brought up a step at a time - an earlier attempt lost the device on
// AMD's Windows driver the moment the video was drawn.
//   0  the frame is turned into RGBA on the CPU
//   1  as 0, and the video pipeline is built, unused
//   2  as 1, and the planes go up to the GPU as well, unused
//   3  the planes alone go up and the video pipeline converts them (the
//      default: all three steps held on the 4500U's AMD driver)
int VideoGpuStage() {
  static const int stage = [] {
    const char* text = std::getenv("XERENGE_VIDEO_GPU");
    return text && *text ? std::clamp(std::atoi(text), 0, 3) : 3;
  }();
  return stage;
}

// Where a video plane's packed copy lives: apart from any guest texture at
// the same address, which the usual texture path builds in its own shape.
uint64_t PackedPlaneKey(uint64_t key) {
  return key ^ 0x9E3779B97F4A7C15ull;
}

// A plane's bytes as RGBA8 texels, four bytes to one - video.frag takes them
// apart again.
uint32_t PackedPlaneWidth(uint32_t width, uint32_t bytes_per_sample) {
  return (width * bytes_per_sample + 3) / 4;
}

struct GuestPlane {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_bytes = 0;
  uint32_t bytes_per_texel = 1;
};

// From `source`, the plane's memory copied when the frame was drawn, when
// there is one; from guest memory as it stands now otherwise.
bool LoadGuestPlane(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                    rex::memory::Memory* memory, const std::vector<uint8_t>& source,
                    GuestPlane* out) {
  if (!memory || !out) {
    return false;
  }
  rex::graphics::TextureInfo info{};
  if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
    return false;
  }
  const uint32_t width = info.width + 1;
  const uint32_t height = info.height + 1;
  if (width == 0 || height == 0 || width > 2048 || height > 2048) {
    return false;
  }
  const uint8_t* src = nullptr;
  if (!source.empty()) {
    if (source.size() < GuestTextureSourceSize(info)) {
      return false;
    }
    src = source.data();
  } else {
    src = memory->TranslatePhysical<const uint8_t*>(info.memory.base_address);
  }
  if (!src) {
    return false;
  }
  uint32_t row_texels = 0;
  if (!UntileGuestTexture(info, src, &out->pixels, &out->row_bytes, &row_texels, false) ||
      out->pixels.empty() || out->row_bytes == 0) {
    return false;
  }
  out->width = width;
  out->height = height;
  const rex::graphics::FormatInfo* fi = info.format_info();
  out->bytes_per_texel = (fi && fi->bytes_per_block()) ? fi->bytes_per_block() : 1;
  return true;
}

uint8_t SampleR8(const GuestPlane& plane, uint32_t x, uint32_t y) {
  if (plane.width == 0 || plane.height == 0 || plane.pixels.empty()) {
    return 128;
  }
  x = std::min(x, plane.width - 1);
  y = std::min(y, plane.height - 1);
  const size_t offset = size_t(y) * plane.row_bytes + size_t(x) * plane.bytes_per_texel;
  if (offset >= plane.pixels.size()) {
    return 128;
  }
  return plane.pixels[offset];
}

bool CompositeYuvToRgba(const GuestPlane& y_plane, const GuestPlane& u_plane,
                        const GuestPlane& v_plane, bool packed_uv, std::vector<uint8_t>* rgba,
                        uint32_t* row_bytes, uint32_t* row_texels) {
  if (!rgba || !row_bytes || !row_texels || y_plane.width == 0 || y_plane.height == 0) {
    return false;
  }
  const uint32_t width = y_plane.width;
  const uint32_t height = y_plane.height;
  const uint32_t dst_row = rex::align(width * 4u, 256u);
  rgba->assign(size_t(dst_row) * height, 0);

  // The chroma planes are smaller than the luma one, so every pixel needs its
  // column mapped into them. Done inline that was two 64-bit divisions per
  // pixel - at a million pixels a frame, tens of millions of cycles, and
  // measured at 11 ms per frame on the guest thread, which both stalled the
  // title and held the video to about six frames a second. The mapping depends
  // only on the widths, so it is the same for every row: build it once.
  std::vector<uint32_t> u_column(width);
  std::vector<uint32_t> v_column(width);
  for (uint32_t x = 0; x < width; ++x) {
    u_column[x] = u_plane.width ? uint32_t(uint64_t(x) * u_plane.width / width) : 0;
    v_column[x] = v_plane.width ? uint32_t(uint64_t(x) * v_plane.width / width) : u_column[x];
  }

  const uint32_t uv_texel = std::max(u_plane.bytes_per_texel, 2u);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* dst = rgba->data() + size_t(y) * dst_row;
    const uint32_t uy = u_plane.height ? uint32_t(uint64_t(y) * u_plane.height / height) : 0;
    const uint32_t vy = v_plane.height ? uint32_t(uint64_t(y) * v_plane.height / height) : uy;
    const size_t u_row = size_t(uy) * u_plane.row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      const int luma = SampleR8(y_plane, x, y);
      int cb = 128;
      int cr = 128;
      if (packed_uv) {
        const size_t offset = u_row + size_t(u_column[x]) * uv_texel;
        if (offset + 1 < u_plane.pixels.size()) {
          cb = u_plane.pixels[offset];
          cr = u_plane.pixels[offset + 1];
        }
      } else {
        cb = SampleR8(u_plane, u_column[x], uy);
        cr = SampleR8(v_plane, v_column[x], vy);
      }
      YuvToRgba(luma, cb, cr, dst + x * 4);
    }
  }
  *row_bytes = dst_row;
  *row_texels = dst_row / 4;
  return true;
}

}  // namespace

void CopyGuestVideoPlanes(GuestDrawSnapshot* snap, memory::Memory* memory) {
  if (!snap || !memory) {
    return;
  }
  using rex::graphics::xenos::FetchConstantType;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    snap->video_source[slot].clear();
    const auto fetch = TextureFetchAt(*snap, slot);
    if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
      continue;
    }
    rex::graphics::TextureInfo info{};
    if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
      continue;
    }
    const size_t size = GuestTextureSourceSize(info);
    // A plane of a 2048-square video at most; anything larger is not one.
    if (size == 0 || size > 8u * 1024 * 1024) {
      continue;
    }
    const auto* src = memory->TranslatePhysical<const uint8_t*>(info.memory.base_address);
    if (src) {
      snap->video_source[slot].assign(src, src + size);
    }
  }
}

bool CaptureGuestVideoFrame(GuestDrawSnapshot* snap, memory::Memory* memory) {
  if (!snap || !memory) {
    return false;
  }
  snap->clear_video_frame();
  snap->video_width = 0;
  snap->video_height = 0;
  snap->video_row_bytes = 0;
  snap->video_row_texels = 0;
  snap->video_key = 0;

  using rex::graphics::xenos::FetchConstantType;
  const auto y_fetch = TextureFetchAt(*snap, 0);
  const auto u_fetch = TextureFetchAt(*snap, 1);
  const auto v_fetch = TextureFetchAt(*snap, 2);
  {
    static uint32_t last_dwords[12] = {};
    const uint32_t now_dwords[12] = {y_fetch.dword_0, y_fetch.dword_1, y_fetch.dword_2, y_fetch.dword_3, y_fetch.dword_4, y_fetch.dword_5,
                                     u_fetch.dword_0, u_fetch.dword_1, u_fetch.dword_2, u_fetch.dword_3, u_fetch.dword_4, u_fetch.dword_5};
    if (std::memcmp(last_dwords, now_dwords, sizeof(now_dwords)) != 0) {
      std::memcpy(last_dwords, now_dwords, sizeof(now_dwords));
      REXLOG_INFO("plume: video fetch constants changed: Y {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}  U {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  now_dwords[0], now_dwords[1], now_dwords[2], now_dwords[3], now_dwords[4], now_dwords[5],
                  now_dwords[6], now_dwords[7], now_dwords[8], now_dwords[9], now_dwords[10], now_dwords[11]);
    }
  }
  if (y_fetch.type != FetchConstantType::kTexture || y_fetch.base_address == 0 ||
      !IsLumaFormat(y_fetch.format)) {
    return false;
  }
  const bool packed_uv = IsPackedUvFormat(u_fetch.format) && u_fetch.base_address != 0 &&
                         u_fetch.type == FetchConstantType::kTexture;
  const bool planar_uv = !packed_uv && u_fetch.type == FetchConstantType::kTexture &&
                         v_fetch.type == FetchConstantType::kTexture && u_fetch.base_address != 0 &&
                         v_fetch.base_address != 0 && IsLumaFormat(u_fetch.format) &&
                         IsLumaFormat(v_fetch.format);
  if (!packed_uv && !planar_uv) {
    return false;
  }

  GuestPlane y_plane;
  GuestPlane u_plane;
  GuestPlane v_plane;
  // Two quite different jobs live in here - unswizzling the guest's planes into
  // linear memory, and turning them into RGBA - and they want different fixes,
  // so keep them apart in the measurement.
  const auto load_started = std::chrono::steady_clock::now();
  if (!LoadGuestPlane(y_fetch, memory, snap->video_source[0], &y_plane) || y_plane.width < 320 || y_plane.height < 180) {
    return false;
  }
  if (!LoadGuestPlane(u_fetch, memory, snap->video_source[1], &u_plane)) {
    return false;
  }
  if (planar_uv && !LoadGuestPlane(v_fetch, memory, snap->video_source[2], &v_plane)) {
    return false;
  }

  uint8_t luma_min = 255;
  uint8_t luma_max = 0;
  uint32_t luma_sum = 0;
  uint32_t luma_samples = 0;
  for (uint32_t y = 0; y < y_plane.height; y += std::max(1u, y_plane.height / 16)) {
    for (uint32_t x = 0; x < y_plane.width; x += std::max(1u, y_plane.width / 16)) {
      const uint8_t luma = SampleR8(y_plane, x, y);
      luma_min = std::min(luma_min, luma);
      luma_max = std::max(luma_max, luma);
      luma_sum += luma;
      ++luma_samples;
    }
  }
  const uint32_t luma_range = uint32_t(luma_max) - uint32_t(luma_min);
  const uint32_t luma_mean = luma_samples ? luma_sum / luma_samples : 0;
  if (luma_range < 4) {
    return false;
  }

  const auto load_done = std::chrono::steady_clock::now();
  std::vector<uint8_t> rgba;
  uint32_t row_bytes = 0;
  uint32_t row_texels = 0;
  // From stage 3 the GPU converts the planes, and the CPU does not.
  if (VideoGpuStage() < 3 &&
      !CompositeYuvToRgba(y_plane, u_plane, v_plane, packed_uv, &rgba, &row_bytes, &row_texels)) {
    return false;
  }
  {
    static std::atomic<uint64_t> load_ns{0}, composite_ns{0}, frames{0};
    load_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(load_done - load_started).count(),
        std::memory_order_relaxed);
    composite_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - load_done)
                               .count(),
                           std::memory_order_relaxed);
    const uint64_t n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n % 120) == 0) {
      REXLOG_WARN("plume: video {} frame(s): {} ms unswizzling, {} ms compositing", n,
                  load_ns.load(std::memory_order_relaxed) / 1000000,
                  composite_ns.load(std::memory_order_relaxed) / 1000000);
    }
  }
  snap->video_rgba = std::move(rgba);
  snap->video_width = y_plane.width;
  snap->video_height = y_plane.height;
  snap->video_row_bytes = row_bytes;
  snap->video_row_texels = row_texels;
  snap->video_key = TextureKey(y_fetch);
  snap->video_luma_mean = luma_mean;
  snap->video_luma_range = luma_range;
  // The copies have served: the snapshot is copied again on every present.
  for (auto& source : snap->video_source) {
    std::vector<uint8_t>().swap(source);
  }
  if (VideoGpuStage() >= 2) {
    const auto texels_per_row = [](const GuestPlane& plane) {
      return plane.row_bytes / std::max(plane.bytes_per_texel, 1u);
    };
    snap->video_packed_uv = packed_uv;
    snap->video_y_row_texels = texels_per_row(y_plane);
    snap->video_u_width = u_plane.width;
    snap->video_u_height = u_plane.height;
    snap->video_u_row_texels = texels_per_row(u_plane);
    snap->video_u_key = TextureKey(u_fetch);
    if (!packed_uv) {
      snap->video_v_width = v_plane.width;
      snap->video_v_height = v_plane.height;
      snap->video_v_row_texels = texels_per_row(v_plane);
      snap->video_v_key = TextureKey(v_fetch);
      snap->video_v = std::move(v_plane.pixels);
    }
    snap->video_u = std::move(u_plane.pixels);
    snap->video_y = std::move(y_plane.pixels);
  }
  // Every change of shape, not only the first few frames: a clip of another
  // size is where the planes stop fitting the textures made for the last.
  static uint64_t last_shape = 0;
  const uint64_t shape = (uint64_t(y_plane.width) << 48) ^ (uint64_t(y_plane.height) << 32) ^
                         (uint64_t(y_plane.row_bytes) << 16) ^ (uint64_t(u_plane.width) << 8) ^
                         u_plane.row_bytes ^ (packed_uv ? 1u : 0u) ^ (TextureKey(y_fetch) << 1);
  if (shape != last_shape) {
    last_shape = shape;
    REXLOG_INFO("plume: capture yuv Y={}x{} (row {}) U={}x{} (row {}) V={}x{} (row {}) packed={} "
                "Y at {:08X}",
                y_plane.width, y_plane.height, y_plane.row_bytes, u_plane.width, u_plane.height,
                u_plane.row_bytes, v_plane.width, v_plane.height, v_plane.row_bytes,
                packed_uv ? 1 : 0, uint32_t(y_fetch.base_address) << 12);
  }
  return true;
}

bool IsGuestVideoBlit(const GuestDrawSnapshot& snap, uint64_t* key_out) {
  using rex::graphics::xenos::FetchConstantType;
  const auto y_fetch = TextureFetchAt(snap, 0);
  const auto u_fetch = TextureFetchAt(snap, 1);
  const auto v_fetch = TextureFetchAt(snap, 2);
  if (y_fetch.type != FetchConstantType::kTexture || y_fetch.base_address == 0 ||
      !IsLumaFormat(y_fetch.format)) {
    return false;
  }
  const bool packed_uv = IsPackedUvFormat(u_fetch.format) && u_fetch.base_address != 0 &&
                         u_fetch.type == FetchConstantType::kTexture;
  const bool planar_uv = u_fetch.type == FetchConstantType::kTexture &&
                         v_fetch.type == FetchConstantType::kTexture && u_fetch.base_address != 0 &&
                         v_fetch.base_address != 0 && IsLumaFormat(u_fetch.format) &&
                         IsLumaFormat(v_fetch.format);
  if (!packed_uv && !planar_uv) {
    return false;
  }
  rex::graphics::TextureInfo info{};
  if (!rex::graphics::TextureInfo::Prepare(y_fetch, &info) || (info.width + 1) < 320 ||
      (info.height + 1) < 180) {
    return false;
  }
  if (key_out) {
    *key_out = TextureKey(y_fetch);
  }
  return true;
}

PlumeDrawContext::~PlumeDrawContext() {
  Shutdown();
}

void PlumeDrawContext::Shutdown() {

  ready_ = false;
  null_texture_uploaded_ = false;
  pipelines_.clear();
  pipeline_layouts_.clear();
  {
    std::lock_guard lock(vfetch_mutex_);
    vfetch_by_shader_.clear();
    real_locations_by_shader_.clear();
    resolved_vfetch_by_shader_.clear();
    vfetch_generation_.fetch_add(1, std::memory_order_acq_rel);
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
  if (watched_memory_ && write_watch_handle_) {
    watched_memory_->UnregisterPhysicalMemoryInvalidationCallback(write_watch_handle_);
  }
  write_watch_handle_ = nullptr;
  watched_memory_ = nullptr;
  if (cache_vb_ && cache_mapped_) {
    cache_vb_->unmap();
  }
  cache_mapped_ = nullptr;
  cache_vb_.reset();
  mesh_cache_.clear();
  cache_by_offset_.clear();
  retired_cache_regions_.clear();
  cache_used_ = 0;
  vs_constants_mapped_ = nullptr;
  ps_constants_mapped_ = nullptr;
  shared_constants_mapped_ = nullptr;
  vb_mapped_ = nullptr;
  draw_sets_.clear();
  vs_constants_.reset();
  ps_constants_.reset();
  shared_constants_.reset();
  dummy_vb_.reset();
  guest_textures_.clear();
  next_bindless_ = 1;
  null_texture_staging_.reset();
  identity_lut_view_.reset();
  identity_lut_.reset();
  identity_lut_staging_.reset();
  layered_textures_.clear();
  volume_set_.reset();
  null_cube_view_.reset();
  null_cube_.reset();
  cube_set_.reset();
  null_texture_view_.reset();
  null_texture_.reset();
  passthrough_pipelines_.clear();
  video_pipelines_.clear();
  video_vs_.reset();
  video_ps_.reset();
  passthrough_vs_.reset();
  passthrough_ps_.reset();
  sampler_index_by_key_.clear();
  guest_samplers_.clear();
  next_sampler_ = 1;
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
#ifdef __ANDROID__
  // Every frame frees a few thousand snapshots' constant blocks and allocates
  // as many again. Android's allocator gave the memory back to the system
  // (madvise) as it was freed - a tenth of the present thread on the Pixel;
  // with a decay time it keeps it a while for the next frame to reuse.
  mallopt(M_DECAY_TIME, 1);
#endif

  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);

  // The textures and samplers the draws refer to by index, kept on the
  // processor (BindlessTable): 2D, layered, cube.
  texture_set_ = std::make_unique<BindlessTable>(kBindlessTextureCount);
  volume_set_ = std::make_unique<BindlessTable>(kBindlessTextureCount);
  cube_set_ = std::make_unique<BindlessTable>(kBindlessTextureCount);
  sampler_set_ = std::make_unique<BindlessTable>(kBindlessSamplerCount);

  // Set 0, the only one (a phone's driver may allow no more than four): the
  // draw's vertex, pixel and shared constants as uniform buffers bound at its
  // slots by dynamic offsets, then its sampler slots - sixteen 2D, layered and
  // cube textures and sixteen samplers (shader_common.h). Plain Vulkan 1.0:
  // no buffer device address, no descriptor indexing.
  draw_set_builder_.begin();
  draw_set_builder_.addConstantBufferDynamic(0);
  draw_set_builder_.addConstantBufferDynamic(1);
  draw_set_builder_.addConstantBufferDynamic(2);
  draw_set_builder_.addTexture(3, kDrawSlots);
  draw_set_builder_.addTexture(4, kDrawSlots);
  draw_set_builder_.addTexture(5, kDrawSlots);
  draw_set_builder_.addSampler(6, kDrawSlots);
  draw_set_builder_.end();
  layout_builder.addDescriptorSet(draw_set_builder_);
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

  // BC (DXT) textures, which the title keeps most of its own in: desktop GPUs
  // and Adreno take them, Mali and others do not.
  {
    const auto probe = device_->createTexture(
        plume::RenderTextureDesc::Texture2D(4, 4, 1, plume::RenderFormat::BC1_UNORM));
    bc_supported_ = probe && VulkanTextureOk(probe.get());
    if (!bc_supported_) {
      // Phones take ETC2 instead: re-encoded on the processor, still 4 bits a texel.
      // XERENGE_NO_ETC2=1 keeps the RGBA8 fallback, for comparing the two.
      const auto etc2 = std::getenv("XERENGE_NO_ETC2") == nullptr
                            ? device_->createTexture(plume::RenderTextureDesc::Texture2D(
                                  4, 4, 1, plume::RenderFormat::ETC2_RGBA8_UNORM))
                            : nullptr;
      etc2_supported_ = etc2 && VulkanTextureOk(etc2.get());
      REXLOG_INFO("plume: this GPU has no BC (DXT) textures; they are {} on the processor",
                  etc2_supported_ ? "re-encoded as ETC2" : "decoded to RGBA8");
    }
  }

  if (volume_set_) {
    plume::RenderTextureDesc lut_desc = plume::RenderTextureDesc::Texture(
        plume::RenderTextureDimension::TEXTURE_2D, kIdentityLutSize, kIdentityLutSize, 1, 1,
        kIdentityLutSize, plume::RenderFormat::R8G8B8A8_UNORM);
    lut_desc.committed = true;
    identity_lut_ = device_->createTexture(lut_desc);
    if (identity_lut_) {
      identity_lut_view_ = identity_lut_->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    }
    const uint64_t lut_bytes = uint64_t(kIdentityLutSize) * kIdentityLutSize * kIdentityLutSize * 4;
    identity_lut_staging_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(lut_bytes));
    if (identity_lut_ && identity_lut_view_ && identity_lut_staging_) {
      if (auto* texels = static_cast<uint8_t*>(identity_lut_staging_->map())) {
        // Across and down are filtered, so each texel holds its own centre;
        // the layer is picked by rounding, so each holds its own index.
        const uint32_t n = kIdentityLutSize;
        for (uint32_t layer = 0; layer < n; ++layer) {
          for (uint32_t y = 0; y < n; ++y) {
            for (uint32_t x = 0; x < n; ++x) {
              uint8_t* t = texels + ((size_t(layer) * n + y) * n + x) * 4;
              t[0] = uint8_t(std::lround((x + 0.5) / n * 255.0));
              t[1] = uint8_t(std::lround((y + 0.5) / n * 255.0));
              t[2] = uint8_t(std::lround(double(layer) / n * 255.0));
              t[3] = 255;
            }
          }
        }
        identity_lut_staging_->unmap();
      }
      for (uint32_t i = 0; i < kBindlessTextureCount; ++i) {
        volume_set_->setTexture(i, identity_lut_.get(), plume::RenderTextureLayout::SHADER_READ,
                                identity_lut_view_.get());
      }
    } else {
      REXLOG_WARN("plume: identity colour table not created; volume textures stay empty");
      identity_lut_view_.reset();
      identity_lut_.reset();
      identity_lut_staging_.reset();
      volume_set_.reset();
    }
  }

  if (cube_set_) {
    plume::RenderTextureDesc cube_desc = plume::RenderTextureDesc::Texture(
        plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
        plume::RenderFormat::R8G8B8A8_UNORM, plume::RenderTextureFlag::CUBE);
    cube_desc.committed = true;
    null_cube_ = device_->createTexture(cube_desc);
    if (null_cube_) {
      null_cube_view_ = null_cube_->createTextureView(
          plume::RenderTextureViewDesc::TextureCube(plume::RenderFormat::R8G8B8A8_UNORM));
    }
    if (null_cube_ && null_cube_view_) {
      for (uint32_t i = 0; i < kBindlessTextureCount; ++i) {
        cube_set_->setTexture(i, null_cube_.get(), plume::RenderTextureLayout::SHADER_READ,
                              null_cube_view_.get());
      }
    } else {
      REXLOG_WARN("plume: null cube map not created; cube maps stay unbound");
      null_cube_view_.reset();
      null_cube_.reset();
      cube_set_.reset();
    }
  }

  null_texture_staging_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (null_texture_staging_) {
    if (void* pixels = null_texture_staging_->map()) {
      const uint8_t empty[4] = {0, 0, 0, 0};
      std::memcpy(pixels, empty, sizeof(empty));
      null_texture_staging_->unmap();
    }
  }

  auto make_cb = [&](uint64_t size, const char* tag) -> std::unique_ptr<plume::RenderBuffer> {
    auto buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        size, plume::RenderBufferFlag::CONSTANT));
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
  if (std::getenv("XERENGE_NO_MESH_CACHE") == nullptr) {
    cache_vb_ = device_->createBuffer(plume::RenderBufferDesc::VertexBuffer(
        uint64_t(kCacheVertexCount) * kVertexStrideBytes, plume::RenderHeapType::UPLOAD));
    if (cache_vb_ && VulkanBufferOk(cache_vb_.get())) {
      cache_mapped_ = static_cast<float*>(cache_vb_->map());
      cache_view_ =
          plume::RenderVertexBufferView(cache_vb_.get(), kCacheVertexCount * kVertexStrideBytes);
    }
    if (!cache_mapped_) {
      REXLOG_WARN("plume: vertex cache unavailable, every draw is unpacked every frame");
      cache_vb_.reset();
    }
  }

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

  // Pipelines are built lazily per (topology, blend state) in PassthroughFor.
  // Warm the common case so a failure here is still reported at init time
  // rather than silently dropping the first frame's draws.
  return PassthroughFor(plume::RenderPrimitiveTopology::TRIANGLE_LIST, kDefaultBlendControl, 0xFu,
                        0) != nullptr;
}

plume::RenderPipeline* PlumeDrawContext::PassthroughFor(plume::RenderPrimitiveTopology topology,
                                                        uint32_t blend_control,
                                                        uint32_t color_mask,
                                                        uint32_t depth_control) {
  return ScreenPipelineFor(passthrough_vs_.get(), passthrough_ps_.get(), passthrough_pipelines_,
                           "passthrough", topology, blend_control, color_mask, depth_control);
}

plume::RenderPipeline* PlumeDrawContext::VideoPipelineFor(plume::RenderPrimitiveTopology topology,
                                                          uint32_t blend_control,
                                                          uint32_t color_mask,
                                                          uint32_t depth_control) {
  if (!video_vs_ || !video_ps_) {
    if (!device_) {
      return nullptr;
    }
    video_vs_ = device_->createShader(kVideoVsSpirv, kVideoVsSpirv_count * 4, "main",
                                      plume::RenderShaderFormat::SPIRV);
    video_ps_ = device_->createShader(kVideoPsSpirv, kVideoPsSpirv_count * 4, "main",
                                      plume::RenderShaderFormat::SPIRV);
    REXLOG_INFO("plume: video shaders {}", video_vs_ && video_ps_ ? "created" : "FAILED");
  }
  return ScreenPipelineFor(video_vs_.get(), video_ps_.get(), video_pipelines_, "video", topology,
                           blend_control, color_mask, depth_control);
}

uint32_t PlumeDrawContext::VideoPlaneSlot(uint64_t key) const {
  auto it = guest_textures_.find(PackedPlaneKey(key));
  if (it == guest_textures_.end() || !it->second || !it->second->view ||
      it->second->format != plume::RenderFormat::R8G8B8A8_UNORM) {
    return 0;
  }
  return it->second->bindless;
}

bool PlumeDrawContext::UsesVideoPipeline(const GuestDrawSnapshot& snap) const {
  if (VideoGpuStage() < 3 || snap.video_y.empty()) {
    return false;
  }
  return VideoPlaneSlot(snap.video_key) != 0 && VideoPlaneSlot(snap.video_u_key) != 0 &&
         (snap.video_packed_uv || VideoPlaneSlot(snap.video_v_key) != 0);
}

plume::RenderPipeline* PlumeDrawContext::ScreenPipelineFor(
    plume::RenderShader* vs, plume::RenderShader* ps,
    std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>>& cache, const char* what,
    plume::RenderPrimitiveTopology topology, uint32_t blend_control, uint32_t color_mask,
    uint32_t depth_control) {
  if (!device_ || !vs || !ps) {
    return nullptr;
  }
  const uint8_t write_mask = WriteMaskForRt0(color_mask);
  const GuestDepthState depth = DepthStateFromGuest(depth_control);
  // Only the parts of the depth state that reach the pipeline take part in the
  // key, so draws differing in stencil bits we ignore still share one.
  const uint64_t depth_key = (depth.enabled ? 1u : 0u) | (depth.write ? 2u : 0u) |
                             (uint64_t(uint32_t(depth.function)) << 2);
  const uint64_t key = (depth_key << 48) | (uint64_t(uint32_t(topology)) << 40) |
                       (uint64_t(write_mask) << 32) | blend_control;
  if (auto it = cache.find(key); it != cache.end()) {
    return it->second.get();
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = pipeline_layout_.get();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.primitiveTopology = topology;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.frontFace = plume::RenderFrontFace::CLOCKWISE;
  desc.depthEnabled = depth.enabled;
  desc.depthWriteEnabled = depth.write;
  desc.depthFunction = depth.function;
  desc.depthClipEnabled = false;
  // Declared whether or not the test is on: the framebuffer carries a depth
  // attachment either way, and the pipeline has to agree with it.
  desc.depthTargetFormat = kPlumeDepthFormat;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kColorTargetFormat;
  desc.renderTargetBlend[0] = BlendDescFromGuest(blend_control, write_mask);
  if (PlumeSecondTargetEnabled()) {
    // Declared, never written: this shader has no second output.
    desc.renderTargetCount = 2;
    desc.renderTargetFormat[1] = kColorTargetFormat;
    desc.renderTargetBlend[1] = BlendDescFromGuest(0x00010001u, 0);
  }
  desc.inputSlots = &input_slots_[0];
  desc.inputSlotsCount = 1;
  desc.inputElements = input_elements_.data();
  desc.inputElementsCount = kInputLocationCount;

  auto pipeline = device_->createGraphicsPipeline(desc);
  if (!pipeline || !VulkanPipelineOk(pipeline.get())) {
    static uint32_t fail_logs = 0;
    if (fail_logs < 8) {
      ++fail_logs;
      REXLOG_ERROR("plume: {} pipeline failed topo={} blend={:08X} mask={:X}", what,
                   uint32_t(topology), blend_control, write_mask);
    }
    return nullptr;
  }
  static uint32_t blend_logs = 0;
  if (blend_logs < 16) {
    ++blend_logs;
    REXLOG_INFO("plume: {} pipeline topo={} blend={:08X} mask={:X}", what, uint32_t(topology),
                blend_control, write_mask);
  }
  plume::RenderPipeline* raw = pipeline.get();
  cache.emplace(key, std::move(pipeline));
  return raw;
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
  // The locations our parser assigns are a convention of this renderer; they
  // only have to agree with the passthrough shader. A translated shader
  // declares its own, inherited from the vertex element declarations of the
  // container it came from, and feeding it our numbering leaves it reading
  // zeros. The real numbering is in the translated source.
  //
  // Declaration order is not the answer: a shader can declare an input it
  // never reads, and the declarations are ordered by the container's element
  // array rather than by the program. What does line up is the order the
  // inputs are *consumed* in the body, because that is the order of the
  // vertex fetch instructions - the same order this microcode parse produces.
  // The cache entry's `source` is the container's path, not the shader text;
  // what the text says was read into a table at build time
  // (shader_source_info.h).
  auto resolved = std::make_unique<ResolvedShaderVfetch>();
  resolved->passthrough_attrs = attrs;
  int pos_index = -1;
  for (size_t i = 0; i < attrs.size(); ++i) {
    if (attrs[i].location == 0) {
      pos_index = int(i);
      break;
    }
  }
  if (pos_index >= 0) {
    resolved->passthrough_pos_fetch_const = int32_t(attrs[size_t(pos_index)].fetch_const);
    resolved->passthrough_pos_float = attrs[size_t(pos_index)].location * 4;
  }

  resolved->real_attrs = attrs;
  if (const ShaderSourceInfo* info = FindShaderSourceInfo(shader_hash)) {
    if (info->guest_index) {
      index_instanced_shaders_.insert(shader_hash);
      resolved->is_index_instanced = true;
    }
    if (info->vertex_fetch) {
      vertex_fetch_shaders_.insert(shader_hash);
      resolved->is_vertex_fetch = true;
    }
    if (info->position_scaling) {
      position_scaling_shaders_.insert(shader_hash);
      resolved->is_position_scaling = true;
    }
    const std::vector<uint32_t>& consumed = info->consumed_locations;
    if (!consumed.empty()) {
      std::string listed;
      for (size_t i = 0; i < consumed.size(); ++i) {
        listed += (i ? ", " : "");
        listed += std::to_string(consumed[i]);
      }
      REXLOG_INFO("plume: VS {:016X} consumes input location(s) {} in fetch order", shader_hash,
                  listed);
      real_locations_by_shader_[shader_hash] = consumed;

      for (size_t a = 0; a < resolved->real_attrs.size() && a < consumed.size(); ++a) {
        resolved->real_attrs[a].location = consumed[a];
      }
    }
  }
  if (pos_index >= 0) {
    resolved->real_pos_fetch_const = int32_t(resolved->real_attrs[size_t(pos_index)].fetch_const);
    resolved->real_pos_float = resolved->real_attrs[size_t(pos_index)].location * 4;
  } else {
    resolved->real_pos_fetch_const = resolved->passthrough_pos_fetch_const;
    resolved->real_pos_float = resolved->passthrough_pos_float;
  }
  REXLOG_INFO("plume: VS {:016X} parsed {} vfetch attr(s)", shader_hash, attrs.size());
  for (size_t i = 0; i < attrs.size() && i < 4; ++i) {
    const VfetchAttr& a = attrs[i];
    REXLOG_INFO("plume:   attr{} fetch={} stride={} off={} fmt={}", a.location, a.fetch_const,
                a.stride_dwords, a.offset_dwords, a.format);
  }
  vfetch_by_shader_.emplace(shader_hash, std::move(attrs));
  resolved_vfetch_by_shader_.emplace(shader_hash, std::move(resolved));
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

std::string PlumeDrawContext::DescribeVsWrapperElements(const uint8_t* bytes, uint32_t byte_size) {
  const std::vector<VfetchAttr> attrs = ParseVfetches(bytes, byte_size);
  if (attrs.empty()) {
    return {};
  }
  // ParseVfetches assigns XenosRecomp's own USAGE_LOCATIONS: POSITION0 at 0,
  // TEXCOORD<n> from 13, COLOR<n> from 17. The wrapper wants those back as
  // semantic names, and the address it wants is the vertex fetch constant the
  // instruction reads.
  std::string out;
  uint32_t texcoords = 0;
  uint32_t colors = 0;
  for (const VfetchAttr& attr : attrs) {
    const char* usage = nullptr;
    uint32_t usage_index = 0;
    if (attr.location == 0) {
      usage = "position";
    } else if (attr.location >= 17) {
      usage = "color";
      usage_index = colors++;
    } else {
      usage = "texcoord";
      usage_index = texcoords++;
    }
    // Keyed by the instruction's own address: a declaration answers "what does
    // the fetch at this point in the program read", so every vertex fetch
    // instruction needs one, and two instructions reading the same fetch
    // constant still need one each.
    out += " --element ";
    out += std::to_string(attr.instr_address);
    out += ':';
    out += usage;
    out += ':';
    out += std::to_string(usage_index);
  }

  // The wrapper takes interpolators as USAGE_INDEX:USAGE:REGISTER. The
  // register is real information and comes straight out of the shader's own
  // export instructions. The semantic name is not in the microcode - it lived
  // in the container this shader never had - but it is only a label: what
  // actually links a vertex shader to a pixel shader here is the register
  // number, and the pixel wrapper declares no interpolator semantics at all,
  // so matching is positional either way. Naming interpolator N as texcoord N
  // keeps that correspondence exact and self-consistent.
  for (uint32_t reg : ParseVsExportRegisters(bytes, byte_size)) {
    out += " --interpolator ";
    out += std::to_string(reg);
    out += ":texcoord:";
    out += std::to_string(reg);
  }

  // A vertex shader may sample textures too, and the stock wrapper cannot say
  // so - it declares constants only. Without these the recompiled code refers
  // to an s<N> that was never defined and the translation fails to compile.
  for (uint32_t reg : ParseSamplerRegisters(bytes, byte_size)) {
    out += " --sampler ";
    out += std::to_string(reg);
  }
  return out;
}

std::string PlumeDrawContext::DescribePsWrapperInterpolators(const uint8_t* bytes,
                                                             uint32_t byte_size) {
  std::string out;
  for (uint32_t reg : ParsePsInterpolatorRegisters(bytes, byte_size)) {
    // Same naming as the vertex side: interpolator register N is texcoord N.
    // The two lists have to agree, because XenosRecomp links a vertex output
    // to a pixel input by the name built from (usage, usageIndex), not by the
    // register number.
    out += " --interpolator ";
    out += std::to_string(reg);
    out += ":texcoord:";
    out += std::to_string(reg);
  }
  // Which colour targets it exports (registers 0-3). Left out, the wrapper
  // declares COLOR0 alone, and a shader that also writes oC1 or oC2 does not
  // compile - one of those was left out of the cache entirely.
  uint32_t outputs = 0;
  for (uint32_t reg : ParseVsExportRegisters(bytes, byte_size)) {
    if (reg < 4) {
      outputs |= 1u << reg;
    }
  }
  if (outputs > 1) {
    out += " --outputs ";
    out += std::to_string(outputs);
  }
  return out;
}

void PlumeDrawContext::RecordShaderCoverage(uint64_t vs_hash, uint64_t ps_hash) {
  // Fire the capability check the moment the car's own shader is the one being
  // used. That is the latest point at which it is certainly loaded and the
  // earliest at which the answer means anything - and the process hard-exits,
  // so waiting for shutdown never runs it at all.
  if (vs_hash == 0x01392C0F9B39E17Eull) {
    Run3DCapabilityCheck();
  }

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  auto note = [&](std::unordered_map<uint64_t, bool>& into, uint64_t hash, const char* kind) {
    if (hash == 0) {
      return false;
    }
    auto it = into.find(hash);
    if (it == into.end()) {
      const bool present = cache.Find(hash) != nullptr;
      it = into.emplace(hash, present).first;
      if (!present) {
        // Reported the moment a draw first needs it, rather than waiting for
        // the periodic summary - this is the event that decides whether the
        // real-shader path can serve a draw at all, and a title loads its
        // heavier shaders exactly when the screen that needs them appears.
        REXLOG_WARN("plume: draw needs untranslated {} {:016X} - geometry using it cannot leave "
                    "the passthrough shader",
                    kind, hash);
      }
    }
    return it->second;
  };
  const bool have_vs = note(vs_coverage_, vs_hash, "VS");
  const bool have_ps = note(ps_coverage_, ps_hash, "PS");
  ++draws_seen_;
  if (have_vs && have_ps) {
    ++draws_both_shaders_;
  }

  // Periodic, because coverage is a property of the whole run: a title can
  // look fully covered on a menu and not at all once a track loads.
  if ((draws_seen_ % 20000) != 0) {
    return;
  }
  auto covered = [](const std::unordered_map<uint64_t, bool>& m) {
    size_t n = 0;
    for (const auto& [hash, present] : m) {
      (void)hash;
      n += present ? 1 : 0;
    }
    return n;
  };
  REXLOG_INFO(
      "plume: shader coverage - VS {}/{} distinct, PS {}/{} distinct, "
      "draws with both {}/{} ({}%)",
      covered(vs_coverage_), vs_coverage_.size(), covered(ps_coverage_), ps_coverage_.size(),
      draws_both_shaders_, draws_seen_,
      draws_seen_ ? (draws_both_shaders_ * 100 / draws_seen_) : 0);
}

plume::RenderPipeline* PlumeDrawContext::GetOrCreatePipeline(
    uint64_t vs_hash, uint64_t ps_hash, plume::RenderPrimitiveTopology topology,
    uint32_t blend_control, uint32_t color_mask, uint32_t depth_control, bool alpha_test,
    bool depth_bias, uint32_t cull) {
  // XERENGE_CULL=1: the title's back-face culling, which is otherwise off - see
  // the cull mode below.
  static const bool culling = std::getenv("XERENGE_CULL") != nullptr;
  cull = culling ? (cull & 7u) : 0u;
  if ((cull & 3u) == 0) {
    cull = 0;
  }
  const GuestDepthState key_depth = DepthStateFromGuest(depth_control);
  const uint32_t state = (blend_control & 0xFFFFu) | (WriteMaskForRt0(color_mask) << 16) |
                         ((key_depth.enabled ? 1u : 0u) << 20) |
                         ((key_depth.write ? 1u : 0u) << 21) |
                         (uint32_t(key_depth.function) << 22) |
                         (uint32_t(WriteMaskForRt0(color_mask >> 4)) << 26) |
                         ((alpha_test ? 1u : 0u) << 30) | ((depth_bias ? 1u : 0u) << 31);
  PipelineKey key{vs_hash, ps_hash, uint32_t(topology), state, (blend_control >> 16) | (cull << 16)};
  // XERENGE_PIPELINE_LOG=1: every pipeline's full guest blend state.
  static const bool pipeline_log = std::getenv("XERENGE_PIPELINE_LOG") != nullptr;
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second.get();
  }
  if (pipeline_log) {
    rex::graphics::reg::RB_BLENDCONTROL bc;
    bc.value = blend_control;
    const auto constant = [](auto f) { return uint32_t(f) >= 12 && uint32_t(f) <= 15; };
    const bool uses_constant = constant(bc.color_srcblend) || constant(bc.color_destblend) ||
                               constant(bc.alpha_srcblend) || constant(bc.alpha_destblend);
    REXLOG_INFO("plume: new pipeline vs={:016X} ps={:016X} topo={} blend={:08X} mask={:X} "
                "depth={:08X} alphatest={}{}",
                vs_hash, ps_hash, uint32_t(topology), blend_control, color_mask, depth_control,
                alpha_test ? 1 : 0, uses_constant ? " USES BLEND CONSTANT" : "");
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
  // The title's culling (PA_SU_SC_MODE_CNTL): bit 0 culls front faces, bit 1
  // back faces, bit 2 makes clockwise the front. Suspected source of the blue
  // bits on an overturned car's underside: the car body, drawn over it with
  // nothing culled. XERENGE_CULL_FLIP=1 swaps which winding is the front, in
  // case the host's viewport reverses it.
  if (cull != 0) {
    static const bool flip = std::getenv("XERENGE_CULL_FLIP") != nullptr;
    const bool front_cw = (((cull >> 2) & 1u) != 0) != flip;
    desc.cullMode = (cull & 1u) ? plume::RenderCullMode::FRONT : plume::RenderCullMode::BACK;
    desc.frontFace = front_cw ? plume::RenderFrontFace::CLOCKWISE
                              : plume::RenderFrontFace::COUNTER_CLOCKWISE;
    static std::mutex seen_mutex;
    static std::set<uint32_t> seen;
    std::lock_guard lock(seen_mutex);
    if (seen.insert(cull).second) {
      REXLOG_INFO("plume: culling {} faces, front {} (PA_SU_SC_MODE_CNTL bits {})",
                  (cull & 1u) ? "front" : "back", front_cw ? "clockwise" : "counter-clockwise",
                  cull);
    }
  }
  const GuestDepthState depth = DepthStateFromGuest(depth_control);
  desc.depthEnabled = depth.enabled;
  desc.depthWriteEnabled = depth.write;
  desc.depthFunction = depth.function;
  desc.depthClipEnabled = false;
  // The bias itself is set per draw (setDepthBias): its values vary by decal.
  desc.dynamicDepthBiasEnabled = depth_bias;
  // The framebuffer carries a depth attachment whether or not the test is on,
  // and the pipeline has to agree with it.
  desc.depthTargetFormat = kPlumeDepthFormat;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kColorTargetFormat;
  desc.renderTargetBlend[0] = BlendDescFromGuest(blend_control, WriteMaskForRt0(color_mask));
  if (PlumeSecondTargetEnabled()) {
    // Render target 1 takes the shader's second output, unblended, through
    // the channels RB_COLOR_MASK bits 4-7 allow - and only while the title has
    // a second target bound (the caller clears those bits otherwise), so a
    // pass drawing into one target leaves the other's contents alone.
    desc.renderTargetCount = 2;
    desc.renderTargetFormat[1] = kColorTargetFormat;
    desc.renderTargetBlend[1] = BlendDescFromGuest(
        0x00010001u, PixelShaderWritesOc1(ps_hash) ? WriteMaskForRt0(color_mask >> 4) : 0);
  }
  // Only the locations this vertex shader declares, packed in order.
  const uint32_t layout = TitleVertexLayout(vs_hash);
  std::array<plume::RenderInputElement, kInputLocationCount> elements{};
  uint32_t element_count = 0;
  for (uint32_t loc = 0; loc < kInputLocationCount; ++loc) {
    if ((layout >> loc) & 1u) {
      elements[element_count] = plume::RenderInputElement(
          "TEXCOORD", loc, loc, plume::RenderFormat::R32G32B32A32_FLOAT, 0, element_count * 16);
      ++element_count;
    }
  }
  const plume::RenderInputSlot slot(
      0, std::max(element_count, 1u) * 16, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = element_count;
  // SPEC_CONSTANT_ALPHA_TEST (shader_common.h): the shader clips below
  // g_AlphaThreshold. Without it the tyres' hubs, which are cut out by alpha,
  // were drawn solid in front of the wheel rims.
  // SPEC_CONSTANT_BLOOM_IN_PLACE (1 << 6) with bloom off (XERENGE_NO_BLOOM):
  // the composite takes its bloom term from the pixel itself, so the sky
  // keeps its blue without the glare (XenosRecomp, the texture fetch).
  static const bool bloom_in_place = std::getenv("XERENGE_NO_BLOOM") != nullptr &&
                                     std::getenv("XERENGE_GREY_SKY") == nullptr;
  const plume::RenderSpecConstant spec(
      0, (alpha_test ? (1u << 1) : 0u) | (bloom_in_place ? (1u << 6) : 0u));
  desc.specConstants = &spec;
  desc.specConstantsCount = 1;

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
  pipeline_layouts_[raw] = layout;
  static uint32_t ok_logs = 0;
  if (ok_logs < 8) {
    ++ok_logs;
    REXLOG_INFO("plume: created graphics pipeline vs={:016X} ps={:016X} topo={}", vs_hash, ps_hash,
                uint32_t(topology));
  }
  return raw;
}


// Can this backend build what a scene draw needs at all? Everything it has ever
// been seen to render is interface geometry - screen-space rectangles through
// the passthrough pair - and the one scene draw that ever arrived never reached
// pipeline creation, so the question has never been answered either way.
//
// These are the exact shader pairs and the exact depth word observed on the car
// screen, at triangle-list topology: what the car itself would need. Building
// them here, before any of it depends on the title behaving, separates "the
// title never hands us the geometry" from "we could not have drawn it anyway".
void PlumeDrawContext::Run3DCapabilityCheck() {
  static const bool wanted = std::getenv("XERENGE_3D_CHECK") != nullptr;
  if (!wanted || !ready_) {
    return;
  }
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  struct Pair {
    const char* what;
    uint64_t vs;
    uint64_t ps;
  };
  static const Pair kPairs[] = {
      {"car body", 0x01392C0F9B39E17Eull, 0x72AEF7C7425D2F23ull},
      {"car second", 0x9EBE7B8B2AA03C1Bull, 0xEAEB9456EFB78013ull},
      {"scene common", 0x0E11948850EAEE91ull, 0x3A67C8117BCE92D9ull},
      {"scene alt", 0xCDA24D914109E246ull, 0xBF9D261289F75F76ull},
  };
  // RB_DEPTHCONTROL 00700766: depth test on, depth write on - the state the car
  // was seen setting. Opaque blend, all channels.
  constexpr uint32_t kDepthTestWrite = 0x00700766u;
  constexpr uint32_t kOpaqueBlend = 0x00010001u;
  constexpr uint32_t kAllChannels = 0xFu;

  uint32_t built = 0;
  uint32_t failed = 0;
  for (const Pair& pair : kPairs) {
    for (const auto topology : {plume::RenderPrimitiveTopology::TRIANGLE_LIST,
                                plume::RenderPrimitiveTopology::TRIANGLE_STRIP}) {
      auto* pipeline = GetOrCreatePipeline(pair.vs, pair.ps, topology, kOpaqueBlend, kAllChannels,
                                           kDepthTestWrite);
      if (pipeline) {
        ++built;
      } else {
        ++failed;
        REXLOG_WARN("plume: 3D check: no pipeline for {} vs={:016X} ps={:016X} topo={}", pair.what,
                    pair.vs, pair.ps, uint32_t(topology));
      }
    }
  }
  REXLOG_WARN("plume: 3D check: {} pipeline(s) built, {} failed", built, failed);
}

namespace {
// Vertex shaders that instance by hand (index_instanced_shaders_, found by
// the GUESTINDEX input XenosRecomp gives them): Xenos gives them the raw index in
// r0.x, they split it into the mesh vertex (index mod gNumVertices, c51.x) and
// the instance (the quotient, which picks a transform out of their constant
// array), and every vertex fetch after that reads the mesh vertex. These are
// the ones XenosRecomp gives a GUESTINDEX input (location 19) for r0.x.
constexpr uint32_t kGuestIndexLocation = 19;
constexpr uint32_t kNumVerticesRegister = 51;
}  // namespace

bool PlumeDrawContext::AllocateCacheRegion(uint32_t count, uint32_t* offset) {
  if (count == 0 || count > kCacheVertexCount) {
    return false;
  }
  // Retired places no frame still on the GPU can read are free again.
  std::erase_if(retired_cache_regions_, [&](const RetiredRegion& r) {
    return r.last_used + 1 < frame_serial_;
  });
  uint32_t head = cache_used_;
  bool wrapped = false;
  for (int attempt = 0; attempt < 16; ++attempt) {
    if (head + count > kCacheVertexCount) {
      if (wrapped) {
        return false;
      }
      head = 0;
      wrapped = true;
    }
    const uint32_t end = head + count;
    // The stored meshes the region would overlap.
    auto first = cache_by_offset_.lower_bound(head);
    if (first != cache_by_offset_.begin()) {
      auto before = std::prev(first);
      if (auto e = mesh_cache_.find(before->second);
          e != mesh_cache_.end() && before->first + e->second.count > head) {
        first = before;
      }
    }
    uint32_t blocked_until = 0;
    for (auto it = first; it != cache_by_offset_.end() && it->first < end; ++it) {
      auto e = mesh_cache_.find(it->second);
      // Drawn this frame or the last, which may still be on the GPU.
      if (e != mesh_cache_.end() && e->second.last_used + 1 >= frame_serial_) {
        blocked_until = std::max(blocked_until, it->first + e->second.count);
      }
    }
    for (const RetiredRegion& r : retired_cache_regions_) {
      if (r.offset < end && r.offset + r.count > head) {
        blocked_until = std::max(blocked_until, r.offset + r.count);
      }
    }
    if (blocked_until != 0) {
      // Drawn in this frame: step over it.
      head = blocked_until;
      continue;
    }
    for (auto it = first; it != cache_by_offset_.end() && it->first < end;) {
      mesh_cache_.erase(it->second);
      it = cache_by_offset_.erase(it);
    }
    cache_used_ = end;
    *offset = head;
    return true;
  }
  return false;
}

// The draw's own descriptor set: its constants and its sixteen sampler slots.
// Sets are kept by what they hold - the index of each slot and the version of
// the entry there - and never written again once made: a set may still be in
// a frame on the GPU, and without descriptor indexing (update after bind) a
// set in use must not change. A slot's entry replaced gives a new key.
plume::RenderDescriptorSet* PlumeDrawContext::DrawSetFor(const DrawBindings& bindings) {
  struct KeyEntry {
    uint32_t index;
    uint32_t version;
  };
  std::array<KeyEntry, kDrawSlots * 4> key{};
  const auto version = [](const BindlessTable* table, uint32_t index) {
    return table ? table->at(index).version : 0u;
  };
  for (uint32_t s = 0; s < kDrawSlots; ++s) {
    key[s] = {bindings.tex2d[s], version(texture_set_.get(), bindings.tex2d[s])};
    key[16 + s] = {bindings.layered[s], version(volume_set_.get(), bindings.layered[s])};
    key[32 + s] = {bindings.cube[s], version(cube_set_.get(), bindings.cube[s])};
    key[48 + s] = {bindings.sampler[s], version(sampler_set_.get(), bindings.sampler[s])};
  }
  const uint64_t hash = XXH3_64bits(key.data(), sizeof(key));
  auto it = draw_sets_.find(hash);
  if (it == draw_sets_.end()) {
    DrawSet made;
    made.set = draw_set_builder_.create(device_);
    if (!made.set) {
      static std::atomic<uint32_t> failed{0};
      if (failed.fetch_add(1, std::memory_order_relaxed) < 4) {
        REXLOG_ERROR("plume: a draw's descriptor set could not be made ({} cached)", draw_sets_.size());
      }
      return nullptr;
    }
    made.set->setBuffer(0, vs_constants_.get(), kVsConstantBytes);
    made.set->setBuffer(1, ps_constants_.get(), kPsConstantBytes);
    made.set->setBuffer(2, shared_constants_.get(), kSharedConstantBytes);
    // Flattened descriptor indices: the three buffers, then each binding's sixteen.
    const auto texture = [&](uint32_t descriptor, const BindlessTable* table, uint32_t index) {
      if (!table) {
        return;
      }
      const BindlessTable::Entry& e = table->at(index);
      if (e.texture) {
        made.set->setTexture(descriptor, e.texture, e.layout, e.view);
      }
    };
    for (uint32_t s = 0; s < kDrawSlots; ++s) {
      texture(3 + s, texture_set_.get(), bindings.tex2d[s]);
      texture(3 + kDrawSlots + s, volume_set_.get(), bindings.layered[s]);
      texture(3 + 2 * kDrawSlots + s, cube_set_.get(), bindings.cube[s]);
      const plume::RenderSampler* sampler = sampler_set_ ? sampler_set_->at(bindings.sampler[s]).sampler : nullptr;
      made.set->setSampler(3 + 3 * kDrawSlots + s, sampler ? sampler : sampler_.get());
    }
    ++draw_sets_made_;
    it = draw_sets_.emplace(hash, std::move(made)).first;
  }
  it->second.last_used = frame_serial_;
  return it->second.set.get();
}

// Sets not used for a few frames are dropped once there are many; the frames
// that may still be on the GPU are the last two.
void PlumeDrawContext::TrimDrawSets() {
  static std::atomic<uint64_t> last_report_ms{0};
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  if (now - last_report_ms.load(std::memory_order_relaxed) >= 10000) {
    last_report_ms.store(now, std::memory_order_relaxed);
    REXLOG_INFO("plume: draw descriptor sets: {} cached, {} made so far", draw_sets_.size(), draw_sets_made_);
  }
  if (draw_sets_.size() <= 6144) {
    return;
  }
  std::erase_if(draw_sets_, [&](const auto& kv) { return kv.second.last_used + 3 < frame_serial_; });
}

// With XERENGE_VERTEX_TRACE a hit reads back the first packed vec4 of the
// mesh's first and last vertex from the cache buffer - two small reads from an
// upload heap, slow enough to keep out of normal play - and compares them with
// what was stored. A difference means something wrote over the place while the
// entry still named it: the vertices drawn would be someone else's.
bool PlumeDrawContext::CacheHoldsMesh(uint64_t key, const CachedMesh& entry) const {
  static const bool trace = std::getenv("XERENGE_VERTEX_TRACE") != nullptr;
  if (!trace || entry.packed_floats == 0 || entry.count == 0 || cache_mapped_ == nullptr) {
    return true;
  }
  const float* base = cache_mapped_ + size_t(entry.offset) * kFloatsPerVert;
  float found[8];
  std::memcpy(found, base, 16);
  std::memcpy(found + 4, base + size_t(entry.count - 1) * entry.packed_floats, 16);
  if (std::memcmp(found, entry.fingerprint.data(), sizeof(found)) == 0) {
    return true;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("plume: vertex cache: mesh {:016X} at {} ({} vertices, {} floats each) holds ({},{},{},{}) .. "
                "({},{},{},{}), stored ({},{},{},{}) .. ({},{},{},{})",
                key, entry.offset, entry.count, entry.packed_floats, found[0], found[1], found[2], found[3],
                found[4], found[5], found[6], found[7], entry.fingerprint[0], entry.fingerprint[1],
                entry.fingerprint[2], entry.fingerprint[3], entry.fingerprint[4], entry.fingerprint[5],
                entry.fingerprint[6], entry.fingerprint[7]);
  }
  return false;
}

// Whether a draw's vertices can be cached, under which key, and whether the
// cached copy still matches the title's buffers. Only the scene's draws with
// the title's own shaders qualify - nothing afterwards rewrites their
// vertices - and not the passes over a copy, whose positions are converted
// depending on whether the copy is this frame's. The hand-instanced props'
// vertices also depend on their mesh size (c51), which goes into the key.
PlumeDrawContext::MeshCheck PlumeDrawContext::CheckMeshCache(
    const GuestDrawSnapshot& snap, const std::vector<VfetchAttr>& attrs, uint32_t vertex_count,
    memory::Memory* memory) const {
  MeshCheck check;
  if (cache_mapped_ == nullptr || memory == nullptr || snap.d3d_vertex_buffer == 0 ||
      snap.d3d_vertex_stride == 0 || attrs.empty() || vertex_count != snap.num_indices ||
      vertex_count > kCacheVertexCount ||
      (snap.d3d_index_buffer == 0 && ReadsResolvedCopy(snap, false))) {
    return check;
  }
  const auto physical_of_guest = [](uint32_t guest) -> uint32_t {
    return (guest & 0x1FFFFFFFu) + (((guest >> 20) + 0x200u) & 0x1000u);
  };
  std::array<uint32_t, 20 + 3 * 32> key{};
  size_t n = 0;
  key[n++] = uint32_t(snap.vs_hash);
  key[n++] = uint32_t(snap.vs_hash >> 32);
  key[n++] = snap.prim_type;
  key[n++] = snap.num_indices;
  key[n++] = snap.d3d_start_index;
  key[n++] = snap.d3d_base_vertex;
  key[n++] = snap.d3d_index_buffer;
  key[n++] = snap.d3d_index_32bit ? 1u : 0u;
  key[n++] = snap.d3d_vertex_buffer;
  key[n++] = snap.d3d_vertex_stride;
  key[n++] = snap.vs_constants[kNumVerticesRegister * 4];
  for (uint32_t i = 0; i < 4; ++i) {
    key[n++] = snap.d3d_stream_address[i];
    key[n++] = snap.d3d_stream_stride[i];
  }
  for (size_t i = 0; i < attrs.size() && i < 32; ++i) {
    std::memcpy(&key[n],
                &snap.fetch_constants[size_t(attrs[i].fetch_const) * 2 % snap.fetch_constants.size()],
                8);
    n += 2;
    key[n++] = attrs[i].location | (attrs[i].format << 8) | (attrs[i].offset_dwords << 16);
  }
  check.key = XXH3_64bits(key.data(), n * 4);
  const uint32_t index_size = snap.d3d_index_32bit ? 4u : 2u;
  const auto* index_host =
      snap.d3d_index_buffer != 0
          ? memory->TranslatePhysical<const uint8_t*>(physical_of_guest(snap.d3d_index_buffer) +
                                                      snap.d3d_start_index * index_size)
          : nullptr;
  if (snap.d3d_index_buffer != 0 && !index_host) {
    return check;
  }
  check.eligible = true;
  check.index_hash = index_host ? XXH3_64bits(index_host, size_t(snap.num_indices) * index_size) : 0;
  auto it = mesh_cache_.find(check.key);
  if (it == mesh_cache_.end()) {
    return check;
  }
  check.unstable = it->second.changes >= 2;
  if (it->second.count == 0 || it->second.index_hash != check.index_hash) {
    return check;
  }
  uint64_t data_hash = 0;
  for (const auto& [start, end] : it->second.ranges) {
    const auto* host = memory->TranslatePhysical<const uint8_t*>(start);
    if (!host || end <= start || end - start > (64u << 20)) {
      return check;
    }
    data_hash = XXH3_64bits_withSeed(host, end - start, data_hash);
  }
  if (data_hash == it->second.data_hash) {
    // The place must still be this mesh's: an entry whose place was given to
    // other meshes draws their vertices with this mesh's pipeline - triangles
    // burst into black slabs across the road (capture new6, frame 9855: a
    // truck's 1295 vertices read from another mesh's 64-byte records).
    const auto owner = cache_by_offset_.find(it->second.offset);
    const bool owned = owner != cache_by_offset_.end() && owner->second == check.key;
    if (!owned || !CacheHoldsMesh(check.key, it->second)) {
      static std::atomic<uint32_t> stale{0};
      const uint32_t n = stale.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16 || (n & (n - 1)) == 0) {
        REXLOG_WARN("plume: vertex cache: mesh {:016X} (vs={:016X}, {} vertices at {}) {} - drawn from the "
                    "title's buffers instead ({} so far)",
                    check.key, snap.vs_hash, it->second.count, it->second.offset,
                    owned ? "was written over" : "lost its place to another mesh", n);
      }
      return check;
    }
    check.hit = true;
    check.offset = it->second.offset;
    check.count = it->second.count;
  }
  return check;
}

namespace {
// The per-stage timings below read the clock around every draw - some twenty
// reads a draw. Where the kernel's clocksource is not the TSC (HPET on the
// laptop this was measured on) each read is a system call, and those alone
// cost milliseconds a frame. So they are taken only with XERENGE_PLUME_TIMING.
bool PlumeTiming() {
  static const bool on = std::getenv("XERENGE_PLUME_TIMING") != nullptr;
  return on;
}

// Milliseconds for the watchdog and the every-few-seconds reports. The coarse
// clock is read from the vDSO without a system call on any clocksource.
uint64_t CoarseMs() {
#ifdef CLOCK_MONOTONIC_COARSE
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
#endif
}

// Where FillVertices spends its time, in four parts, reported every few
// seconds: the garage ran at twenty frames a second on unpacking alone.
std::atomic<uint64_t> g_fill_ns[4];
void FillPhase(int phase, std::chrono::steady_clock::time_point& mark) {
  if (!PlumeTiming()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  g_fill_ns[phase].fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count(),
      std::memory_order_relaxed);
  mark = now;
}
}  // namespace

// The vertices of one draw, fetched and unpacked from the title's buffers into
// `staged` (vertex_count vertices, zeroed beforehand). Touches nothing shared,
// so the frame's draws can be unpacked on the worker threads at once. When
// read_lo is not empty, the dwords each attribute read are bounded there, for
// the vertex cache's content check.
void PlumeDrawContext::UnpackVertices(const GuestDrawSnapshot& snap,
                                      const std::vector<VfetchAttr>& attrs,
                                      int32_t pos_fetch_const, uint32_t pos_float,
                                      uint32_t vertex_count,
                                      memory::Memory* memory, float* staged,
                                      std::vector<uint32_t>& read_lo,
                                      std::vector<uint32_t>& read_hi, uint32_t& fetched,
                                      uint32_t& fetch_addr, uint32_t& fetch_type) const {
  using rex::graphics::xenos::FetchConstantType;
  using rex::graphics::xenos::PrimitiveType;
  auto vert = [&](uint32_t i) { return staged + i * kFloatsPerVert; };
  const bool cache_eligible = !read_lo.empty();
  const auto physical_of = [](uint32_t guest) -> uint32_t {
    return (guest & 0x1FFFFFFFu) + (((guest >> 20) + 0x200u) & 0x1000u);
  };
  const bool from_d3d = snap.d3d_vertex_buffer != 0 && snap.d3d_vertex_stride != 0;
  const uint32_t d3d_base_dwords =
      from_d3d ? (physical_of(snap.d3d_vertex_buffer) >> 2) : 0u;
  const int32_t d3d_stride_dwords = from_d3d ? int32_t(snap.d3d_vertex_stride / 4) : 0;
  const auto* d3d_indices =
      (from_d3d && snap.d3d_index_buffer != 0)
          ? memory->TranslatePhysical<const uint16_t*>(physical_of(snap.d3d_index_buffer))
          : nullptr;

  // What the index buffer actually holds. Triangles came out scattered, which
  // a strip does when an index is not what it is taken for - a restart
  // marker unrolled into a vertex, or thirty-two bit indices read as sixteen.
  if (d3d_indices && snap.num_indices >= 64) {
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
      uint32_t restarts = 0;
      uint32_t highest = 0;
      for (uint32_t i = 0; i < snap.num_indices; ++i) {
        const uint16_t raw = d3d_indices[snap.d3d_start_index + i];
        const uint32_t v = uint32_t((raw >> 8) | (raw << 8)) & 0xFFFFu;
        if (v == 0xFFFFu) {
          ++restarts;
        } else {
          highest = std::max(highest, v);
        }
      }
      std::string head;
      for (uint32_t i = 0; i < 16 && i < snap.num_indices; ++i) {
        const uint16_t raw = d3d_indices[snap.d3d_start_index + i];
        head += fmt::format(" {:04X}", uint32_t((raw >> 8) | (raw << 8)) & 0xFFFFu);
      }
      REXLOG_INFO("plume: scene indices prim={} count={} start={} base={} restarts={} highest={}:{}",
                  snap.prim_type, snap.num_indices, snap.d3d_start_index, snap.d3d_base_vertex,
                  restarts, highest, head);
    }
  }
  CheckShaderConstants(snap);
  // Hand-instanced shaders: see IsIndexInstancedShader.
  const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
  const bool index_instanced = vs_info ? vs_info->is_index_instanced : false;
  uint32_t mesh_vertices = 0;
  if (index_instanced) {
    const float n = reinterpret_cast<const float*>(snap.vs_constants.data())[kNumVerticesRegister * 4];
    if (n >= 1.0f && n < 1048576.0f) {
      mesh_vertices = uint32_t(n);
    }
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
      REXLOG_INFO("plume: index-instanced draw vs={:016X} mesh vertices {} indices {}",
                  snap.vs_hash, mesh_vertices, snap.num_indices);
    }
  }

  // Pre-prepare invariant attribute parameters once for the entire draw:
  struct PreparedAttr {
    int32_t base_dwords = 0;
    int32_t stride_dwords = 0;
    uint32_t max_dwords = 0;
    uint32_t fetch_address = 0;
    rex::graphics::xenos::Endian endian = rex::graphics::xenos::Endian::kNone;
    float exp_scale = 1.0f;
    bool skip = false;
    bool has_exp_adjust = false;
    bool stream0 = false;
  };

  std::vector<PreparedAttr> prep_attrs(attrs.size());
  for (size_t ai = 0; ai < attrs.size(); ++ai) {
    const VfetchAttr& attr = attrs[ai];
    auto& prep = prep_attrs[ai];
    const auto fetch = FetchAt(snap, attr.fetch_const);
    prep.fetch_address = fetch.address;
    if (!from_d3d && fetch.type != FetchConstantType::kVertex &&
        fetch.type != FetchConstantType::kInvalidVertex) {
      prep.skip = true;
      continue;
    }
    if (attr.location == 0) {
      fetch_addr = from_d3d ? d3d_base_dwords : fetch.address;
      fetch_type = uint32_t(fetch.type);
    }
    const bool fetch_valid = (fetch.type == FetchConstantType::kVertex ||
                              fetch.type == FetchConstantType::kInvalidVertex) &&
                             fetch.address != 0 && attr.stride_dwords != 0;
    const bool index_instanced_secondary =
        index_instanced && mesh_vertices != 0 && fetch_valid &&
        int32_t(attr.stride_dwords) != d3d_stride_dwords;
    const bool other_stream =
        from_d3d && fetch_valid &&
        (snap.d3d_index_buffer != 0 || index_instanced_secondary ||
         (pos_fetch_const >= 0 && attr.fetch_const != uint32_t(pos_fetch_const)));
    const uint32_t stream_index = 95u - attr.fetch_const;
    const bool title_stream = from_d3d && stream_index < 4 &&
                              snap.d3d_stream_address[stream_index] != 0 &&
                              snap.d3d_stream_stride[stream_index] != 0;
    const bool stream0 = from_d3d && !other_stream && !title_stream;
    prep.stream0 = stream0;
    prep.base_dwords = stream0 ? int32_t(d3d_base_dwords) : int32_t(fetch.address);
    prep.stride_dwords = stream0 ? d3d_stride_dwords : int32_t(attr.stride_dwords);
    if (title_stream) {
      prep.base_dwords = int32_t(physical_of(snap.d3d_stream_address[stream_index]) >> 2);
      prep.stride_dwords = int32_t(snap.d3d_stream_stride[stream_index] / 4);
      if (fetch_valid && uint32_t(prep.base_dwords) != fetch.address) {
        static std::mutex mm_mutex;
        static std::set<uint64_t> mm_seen;
        bool fresh = false;
        {
          std::lock_guard lock(mm_mutex);
          fresh = mm_seen.size() < 24 &&
                  mm_seen.insert(snap.vs_hash ^ (uint64_t(attr.fetch_const) << 56)).second;
        }
        if (fresh) {
          REXLOG_WARN("plume: stream {} of vs={:016X}: title {:08X} (stride {}) vs fetch "
                      "constant {:08X} (stride {} dwords)",
                      stream_index, snap.vs_hash, uint32_t(prep.base_dwords) << 2,
                      snap.d3d_stream_stride[stream_index], fetch.address << 2,
                      attr.stride_dwords);
        }
      }
    }
    prep.max_dwords = std::min(4u, attr.stride_dwords);
    prep.endian = stream0 || title_stream ||
                          (other_stream && fetch.endian == rex::graphics::xenos::Endian::kNone)
                      ? rex::graphics::xenos::Endian::k8in32
                      : fetch.endian;
    prep.has_exp_adjust = (attr.exp_adjust != 0);
    prep.exp_scale = prep.has_exp_adjust ? std::ldexp(1.0f, attr.exp_adjust) : 1.0f;
  }

  std::array<float, 64 * 4> regs;
  for (uint32_t vi = 0; vi < vertex_count; ++vi) {
    float* dst = vert(vi);
    // Indexed draws take the vertex from the index buffer; the rest walk the
    // buffer in order. Either way the draw's own first vertex applies - it was
    // being added only on the indexed path, so every non-indexed draw read
    // from the start of the buffer instead of from its own place in it.
    uint32_t source_vertex = vi + snap.d3d_base_vertex;
    if (d3d_indices) {
      // Index width was assumed, never checked. Thirty-two bit indices read as
      // sixteen give every other vertex a value built from two halves of
      // neighbouring indices, which folds the mesh into a wedge.
      // 32-bit where the index buffer says so. The race's world is drawn from
      // 32-bit index buffers; read as 16-bit, every index was half of one,
      // and its meshes pulled vertices from megabytes away.
      static const bool wide_indices = std::getenv("XERENGE_INDEX32") != nullptr;
      if (wide_indices || snap.d3d_index_32bit) {
        const auto* wide = reinterpret_cast<const uint32_t*>(d3d_indices);
        const uint32_t raw = wide[snap.d3d_start_index + vi];
        source_vertex = ((raw >> 24) | ((raw >> 8) & 0xFF00u) | ((raw << 8) & 0xFF0000u) |
                         (raw << 24)) +
                        snap.d3d_base_vertex;
      } else {
        const uint16_t raw = d3d_indices[snap.d3d_start_index + vi];
        // Mask after the shift. A sixteen-bit value promotes to int before
        // shifting, so the high byte moved out of the index rather than off
        // the end of it: index two read as 0x20002, and every vertex was
        // fetched megabytes past the buffer.
        source_vertex =
            (uint32_t((raw >> 8) | (raw << 8)) & 0xFFFFu) + snap.d3d_base_vertex;
      }
    }
    // What Xenos puts in r0.x: the index itself. Attributes written below
    // take precedence should one of them use this location.
    dst[kGuestIndexLocation * 4] = float(source_vertex);
    uint32_t fetch_vertex =
        (index_instanced && mesh_vertices != 0) ? source_vertex % mesh_vertices : source_vertex;
    if (index_instanced && mesh_vertices != 0) {
      regs.fill(0.0f);
      regs[0] = float(fetch_vertex);
      regs[1] = float(source_vertex / mesh_vertices);
    }
    for (size_t ai = 0; ai < attrs.size(); ++ai) {
      const auto& prep = prep_attrs[ai];
      if (prep.skip) {
        continue;
      }
      const VfetchAttr& attr = attrs[ai];
      if (index_instanced && mesh_vertices != 0 && attr.src_reg < 64) {
        const float index = regs[attr.src_reg * 4 + attr.src_comp];
        fetch_vertex = index > 0.0f ? uint32_t(attr.index_rounded ? index + 0.5f : index) : 0u;
      }
      const int32_t dword_addr =
          prep.base_dwords + prep.stride_dwords * int32_t(fetch_vertex) + attr.offset_dwords;
      if (dword_addr < prep.base_dwords) {
        continue;
      }
      if (cache_eligible) {
        read_lo[ai] = std::min(read_lo[ai], uint32_t(dword_addr));
        read_hi[ai] = std::max(read_hi[ai], uint32_t(dword_addr));
      }
      uint32_t data[4] = {};
      for (uint32_t w = 0; w < prep.max_dwords; ++w) {
        const uint32_t phys = uint32_t(dword_addr + int32_t(w)) << 2;
        if (auto* host = memory->TranslatePhysical<uint32_t*>(phys)) {
          data[w] = rex::graphics::xenos::GpuSwap(*host, prep.endian);
        }
      }
      // What actually came out of the buffer for the first vertex of a scene
      // draw. Sane positions mean the addressing is right and the transform is
      // at fault; absurd ones mean we are reading the wrong bytes. Guessing
      // between those two by changing index widths has cost enough runs.
      float unpacked[4];
      UnpackVertex(static_cast<rex::graphics::xenos::VertexFormat>(attr.format), data,
                   attr.is_signed, attr.is_normalized, unpacked);
      // An attribute that comes out as NaN or absurdly large (crash mode: debris drawn by a hand-instanced shader
      // came out as black needles, one attribute of the vertices holding -nan or -2.9e38) is logged once for its
      // shader and location with everything that decides where it was read, and zeroed - a spike in a vertex
      // costs the whole frame its picture. The log line needs XERENGE_VERTEX_TRACE (the launcher's debug_vertex_trace);
      // XERENGE_KEEP_BAD_VERTICES=1 keeps the raw values.
      {
        bool absurd = false;
        for (float x : unpacked) {
          absurd = absurd || !std::isfinite(x) || std::fabs(x) > 1.0e12f;
        }
        if (absurd) {
          static const bool keep = std::getenv("XERENGE_KEEP_BAD_VERTICES") != nullptr;
          static const bool trace = std::getenv("XERENGE_VERTEX_TRACE") != nullptr;
          static std::mutex bad_mutex;
          static std::set<uint64_t> bad_seen;
          bool fresh = false;
          {
            std::lock_guard lock(bad_mutex);
            fresh = bad_seen.size() < 64 &&
                    bad_seen.insert(snap.vs_hash ^ (uint64_t(attr.location) << 58)).second;
          }
          if (trace) {
            TrapArm(uint32_t(dword_addr) << 2);
          }
          if (fresh && trace) {
            REXLOG_WARN(
                "plume: absurd vertex attribute vs={:016X} loc={} fmt={} signed={} norm={} fetch={} stride={} "
                "off={} vertex {} (fetched {}) raw={:08X} {:08X} {:08X} {:08X} -> ({},{},{},{}) at dword {:08X} "
                "base {:08X} stream0={} indexed={} mesh_vertices={} draw indices={}",
                snap.vs_hash, attr.location, uint32_t(attr.format), attr.is_signed, attr.is_normalized,
                attr.fetch_const, attr.stride_dwords, attr.offset_dwords, vi, fetch_vertex, data[0], data[1],
                data[2], data[3], unpacked[0], unpacked[1], unpacked[2], unpacked[3], uint32_t(dword_addr),
                uint32_t(prep.base_dwords), prep.stream0, index_instanced, mesh_vertices, snap.num_indices);
          }
          // The position is left alone here: CollapseBadPrimitives drops the primitives it belongs to, as Xenos does.
          // Zeroing it drew the vertex at the origin, and every triangle of it became a needle.
          if (!keep && attr.location * 4 != pos_float) {
            for (float& x : unpacked) {
              if (!std::isfinite(x) || std::fabs(x) > 1.0e12f) {
                x = 0.0f;
              }
            }
          }
        }
      }
      if (from_d3d && vi == 0 && attr.location == 0) {
        static std::atomic<uint64_t> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
          REXLOG_INFO(
              "plume: scene vertex vs={:016X} raw={:08X} {:08X} {:08X} {:08X} -> "
              "({:.3f},{:.3f},{:.3f},{:.3f}) fmt={} endian={} at {:08X}+{}",
              snap.vs_hash, data[0], data[1], data[2], data[3], unpacked[0], unpacked[1],
              unpacked[2], unpacked[3], uint32_t(attr.format), uint32_t(prep.endian),
              snap.d3d_vertex_buffer, snap.d3d_vertex_stride);
        }
      }
      // Every attribute of the first vertex of a few scene draws, as unpacked.
      // The scene came out in shades of its fog colour, which is what the
      // pixel shader gives when texture times vertex colour is zero - so the
      // colour attribute has to be seen, not just the position.
      if (from_d3d && vi == 0) {
        // Once per shader: each one has its own vertex layout, and a layout
        // this code unpacks wrongly collapses every mesh drawn with it.
        static std::mutex shown_mutex;
        static std::map<uint64_t, uint32_t> shown_per_shader;
        bool show = false;
        {
          std::lock_guard lock(shown_mutex);
          auto& count = shown_per_shader[snap.vs_hash];
          show = shown_per_shader.size() <= 12 && count < attrs.size();
          ++count;
        }
        if (show) {
          REXLOG_INFO("plume: scene attr vs={:016X} loc={} fmt={} off={} signed={} norm={} "
                      "raw={:08X} {:08X} -> ({:.3f},{:.3f},{:.3f},{:.3f})",
                      snap.vs_hash, attr.location, uint32_t(attr.format), attr.offset_dwords,
                      attr.is_signed, attr.is_normalized, data[0], data[1], unpacked[0],
                      unpacked[1], unpacked[2], unpacked[3]);
        }
      }
      // Absurd values mean the attribute is read from the wrong place or in
      // the wrong format; say which, once per shader and location.
      if ((std::fabs(unpacked[0]) > 1e6f || std::fabs(unpacked[1]) > 1e6f ||
           std::fabs(unpacked[2]) > 1e6f || std::isnan(unpacked[0]))) {
        static std::mutex bad_mutex;
        static std::set<uint64_t> bad_seen;
        bool fresh = false;
        {
          std::lock_guard lock(bad_mutex);
          fresh = bad_seen.size() < 40 &&
                  bad_seen.insert(snap.vs_hash ^ (uint64_t(attr.location) << 56)).second;
        }
        if (fresh) {
          std::string all;
          for (const VfetchAttr& a : attrs) {
            all += fmt::format(" [loc{} fc{} fmt{} off{} stride{} sgn{} nrm{}]", a.location,
                               a.fetch_const, a.format, a.offset_dwords, a.stride_dwords,
                               a.is_signed ? 1 : 0, a.is_normalized ? 1 : 0);
          }
          REXLOG_WARN("plume: absurd vertex attr vs={:016X} loc={} raw={:08X} {:08X} {:08X} -> "
                      "({:g},{:g},{:g}) stream0={} d3d={} vb={:08X}+{} fetch addr={:08X} "
                      "endian={} | layout:{}",
                      snap.vs_hash, attr.location, data[0], data[1], data[2], unpacked[0],
                      unpacked[1], unpacked[2], prep.stream0 ? 1 : 0, from_d3d ? 1 : 0,
                      snap.d3d_vertex_buffer, snap.d3d_vertex_stride, prep.fetch_address << 2,
                      uint32_t(prep.endian), all);
        }
      }
      if (prep.has_exp_adjust) {
        unpacked[0] *= prep.exp_scale;
        unpacked[1] *= prep.exp_scale;
        unpacked[2] *= prep.exp_scale;
        unpacked[3] *= prep.exp_scale;
      }
      // Into the register file, through the destination swizzle: 0-3 pick a
      // fetched component, 4 and 5 write 0 and 1, anything else leaves it.
      if (index_instanced && attr.dst_reg < 64) {
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t sel = (attr.dst_swiz >> (c * 3)) & 7u;
          if (sel < 4) {
            regs[attr.dst_reg * 4 + c] = unpacked[sel];
          } else if (sel == 4) {
            regs[attr.dst_reg * 4 + c] = 0.0f;
          } else if (sel == 5) {
            regs[attr.dst_reg * 4 + c] = 1.0f;
          }
        }
      }
      if (attr.location >= kInputLocationCount) {
        continue;
      }
      std::memcpy(dst + attr.location * 4, unpacked, 16);
      if (attr.location == 0) {
        ++fetched;
      }
    }
  }

}

void PlumeDrawContext::CopyVerticesOut(float* dst, const float* staged, uint32_t count) const {
  const uint32_t layout = fill_layout_mask_;
  if (layout == kFullLayoutMask) {
    std::memcpy(dst, staged, size_t(count) * kVertexStrideBytes);
    return;
  }
  uint32_t locations[kInputLocationCount];
  uint32_t n = 0;
  for (uint32_t loc = 0; loc < kInputLocationCount; ++loc) {
    if ((layout >> loc) & 1u) {
      locations[n++] = loc * 4;
    }
  }
  if (n == 0) {
    return;
  }
  // Written front to back, so the write-combined heap still sees one
  // sequential stream.
  float* const start = dst;
  for (uint32_t v = 0; v < count; ++v) {
    const float* from = staged + size_t(v) * kFloatsPerVert;
    for (uint32_t k = 0; k < n; ++k) {
      std::memcpy(dst, from + locations[k], 16);
      dst += 4;
    }
  }
  static const bool check = std::getenv("XERENGE_PACK_CHECK") != nullptr;
  if (check) {
    last_packed_dst_ = start;
    // Read back from the upload heap: slow, which is why it is a check only.
    uint32_t bad = 0;
    uint32_t first_bad = 0;
    for (uint32_t v = 0; v < count; ++v) {
      for (uint32_t k = 0; k < n; ++k) {
        if (std::memcmp(start + (size_t(v) * n + k) * 4,
                        staged + size_t(v) * kFloatsPerVert + locations[k], 16) != 0) {
          if (bad++ == 0) {
            first_bad = v;
          }
        }
      }
    }
    if (bad != 0) {
      static std::atomic<uint32_t> shown{0};
      if (shown.fetch_add(1, std::memory_order_relaxed) < 16) {
        REXLOG_WARN("plume: pack check: copy wrong at once - vs={:016X} {} vertices x {} "
                    "locations, {} wrong, first vertex {}",
                    fill_vs_hash_, count, n, bad, first_bad);
      }
    }
    const size_t bytes = size_t(count) * n * 16;
    packed_writes_.push_back({start, bytes, XXH3_64bits(start, bytes), fill_vs_hash_, count});
  }
}

void PlumeDrawContext::CheckPackedWrites() {
  uint32_t changed = 0;
  for (const PackedWrite& w : packed_writes_) {
    if (XXH3_64bits(w.dst, w.bytes) != w.hash) {
      static std::atomic<uint32_t> shown{0};
      if (shown.fetch_add(1, std::memory_order_relaxed) < 16) {
        const auto* base = reinterpret_cast<const uint8_t*>(w.dst);
        const bool in_cache = cache_mapped_ && w.dst >= cache_mapped_ &&
                              w.dst < cache_mapped_ + size_t(kCacheVertexCount) * kFloatsPerVert;
        const auto* origin = reinterpret_cast<const uint8_t*>(in_cache ? cache_mapped_
                                                                         : vb_mapped_);
        REXLOG_WARN("plume: pack check: vertices overwritten during the frame - vs={:016X} {} "
                    "vertices, {} bytes at {} +{}",
                    w.vs, w.count, w.bytes, in_cache ? "cache" : "frame buffer",
                    size_t(base - origin));
      }
      ++changed;
    }
  }
  static std::atomic<uint64_t> frames{0};
  static std::atomic<uint64_t> total{0};
  total.fetch_add(packed_writes_.size(), std::memory_order_relaxed);
  if (frames.fetch_add(1, std::memory_order_relaxed) % 300 == 0) {
    REXLOG_INFO("plume: pack check: {} packed copies checked so far, {} changed this frame",
                total.load(), changed);
  }
  packed_writes_.clear();
}

uint32_t PlumeDrawContext::FillVertices(const GuestDrawSnapshot& snap, memory::Memory* memory,
                                        uint32_t base_vertex, bool passthrough) {
  auto fill_mark = PlumeTiming() ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  last_fill_cache_offset_ = ~0u;
  last_fill_passthrough_ = passthrough;
  last_fill_x_known_ = false;
  // Every path that refuses a draw says so once. A draw silently returning
  // zero vertices is indistinguishable on screen from one that rendered
  // wrong, and a whole row of UI can sit in a single draw.
  auto refuse = [&](const char* why) -> uint32_t {
    {
      static std::mutex counts_mutex;
      static std::map<std::string, uint64_t> counts;
      static std::atomic<uint64_t> last_ms{0};
      std::lock_guard lock(counts_mutex);
      ++counts[fmt::format("{} vs={:016X} from {}", why, snap.vs_hash,
                           snap.d3d_vertex_buffer != 0 ? "Direct3D" : "ring")];
      const uint64_t now = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      uint64_t was = last_ms.load(std::memory_order_relaxed);
      if (now - was >= 3000 && last_ms.compare_exchange_strong(was, now)) {
        std::string report;
        for (const auto& c : counts) {
          report += fmt::format(" [{} x{}]", c.first, c.second);
        }
        REXLOG_INFO("plume: draws refused so far:{}", report);
      }
    }
    static uint32_t refuse_logs = 0;
    if (refuse_logs < 24) {
      ++refuse_logs;
      REXLOG_INFO("plume: draw refused ({}) vs={:016X} prim={} indices={}", why, snap.vs_hash,
                  snap.prim_type, snap.num_indices);
    }
    return 0;
  };
  if (!vb_mapped_ || snap.num_indices == 0 || base_vertex >= vb_limit_) {
    return refuse("no vb / empty / base past end");
  }
  const uint32_t vertex_count = std::min(snap.num_indices, vb_limit_ - base_vertex);

  static const std::vector<VfetchAttr> kEmptyAttrs;
  const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
  const std::vector<VfetchAttr>& attrs =
      vs_info ? (passthrough ? vs_info->passthrough_attrs : vs_info->real_attrs) : kEmptyAttrs;
  const uint32_t pos_float =
      vs_info ? (passthrough ? vs_info->passthrough_pos_float : vs_info->real_pos_float) : 0;
  const int32_t pos_fetch_const =
      vs_info ? (passthrough ? vs_info->passthrough_pos_fetch_const : vs_info->real_pos_fetch_const)
              : -1;

  // The vertex cache: see cache_vb_ and CheckMeshCache.
  last_fill_cache_offset_ = ~0u;
  bool cache_eligible = false;
  uint64_t cache_key = 0;
  uint64_t index_hash = 0;
  // Cached vertices are kept in the layout of the title's own pipeline for the
  // shader; a draw falling back to another pipeline neither stores nor uses them.
  if (!passthrough && memory && fill_layout_mask_ == TitleVertexLayout(snap.vs_hash)) {
    MeshCheck check;
    if (auto found = mesh_checks_.find(&snap); found != mesh_checks_.end()) {
      check = found->second;
    } else {
      check = CheckMeshCache(snap, attrs, vertex_count, memory);
    }
    // A draw cut short by a nearly full frame buffer is not what the key
    // stands for; a hit is still good, as it comes from the cache.
    if (check.eligible && (check.hit || vertex_count == snap.num_indices)) {
      cache_eligible = true;
      cache_key = check.key;
      index_hash = check.index_hash;
      if (check.hit) {
        ++cache_hits_;
        if (auto e = mesh_cache_.find(check.key); e != mesh_cache_.end()) {
          e->second.last_used = frame_serial_;
        }
        last_fill_cache_offset_ = check.offset;
        FillPhase(0, fill_mark);
        return check.count;
      }
      // A mesh whose vertices change from frame to frame - the wreck as it
      // crumples - is not worth caching; after a couple of changes leave it.
      if (check.unstable) {
        cache_eligible = false;
      }
      ++cache_misses_;
    }
  }
  const auto ranges_hash = [&](const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                               uint64_t* out) {
    uint64_t h = 0;
    for (const auto& [start, end] : ranges) {
      const auto* host = memory->TranslatePhysical<const uint8_t*>(start);
      if (!host || end <= start || end - start > (64u << 20)) {
        return false;
      }
      h = XXH3_64bits_withSeed(host, end - start, h);
    }
    *out = h;
    return true;
  };
  // Bounds of what each attribute read, for the cache's content check.
  std::vector<uint32_t> read_lo;
  std::vector<uint32_t> read_hi;
  if (cache_eligible) {
    read_lo.assign(attrs.size(), ~0u);
    read_hi.assign(attrs.size(), 0u);
  }

  // Build the vertices in ordinary memory and hand them over in one go at the
  // end. The buffer they are destined for lives in an upload heap - write
  // combined - where scattered sixteen-byte writes are expensive and reads are
  // far worse, and this code both writes each attribute separately and reads
  // every vertex back afterwards. Measured on the car-select scene it cost 3.7
  // us per vertex, 29 seconds of a 38 second run, which is the whole reason
  // the scene ran at half a frame a second.
  //
  // Room for the expansions below, which turn rectangles and quads into more
  // vertices than they started with.
  // Unpacked already, on a worker thread, before the frame was encoded?
  const PreUnpacked* pre = nullptr;
  if (!passthrough) {
    if (auto found = pre_unpacked_.find(&snap);
        found != pre_unpacked_.end() && found->second.count == vertex_count) {
      pre = &found->second;
    }
  }
  // Those are worked on where they lie, rather than copied into the staging
  // buffer first - 320 bytes a vertex, a race frame's largest single cost -
  // unless the draw is expanded below, which needs the staging buffer's
  // extra room.
  const auto prim = static_cast<rex::graphics::xenos::PrimitiveType>(snap.prim_type);
  const bool expands = prim == rex::graphics::xenos::PrimitiveType::kRectangleList ||
                       prim == rex::graphics::xenos::PrimitiveType::kQuadList;
  const bool in_place = pre != nullptr && !expands;
  if (!in_place) {
    const size_t staged_floats = size_t(vertex_count) * 2 * kFloatsPerVert;
    if (stage_.size() < staged_floats) {
      stage_.resize(staged_floats);
    }
  }
  float* const staged = in_place ? pre_arena_.data() + pre->offset : stage_.data();
  auto vert = [&](uint32_t i) { return staged + i * kFloatsPerVert; };
  if (!pre) {
    // Only what goes to the GPU needs a defined value: the locations of the
    // layout the vertices are copied out in (CopyVerticesOut). Clearing all
    // twenty of a packed draw's was most of the work.
    if (fill_layout_mask_ == kFullLayoutMask) {
      std::memset(staged, 0, size_t(vertex_count) * kVertexStrideBytes);
    } else {
      uint32_t cleared[kInputLocationCount];
      uint32_t n = 0;
      for (uint32_t loc = 0; loc < kInputLocationCount; ++loc) {
        if ((fill_layout_mask_ >> loc) & 1u) {
          cleared[n++] = loc * 4;
        }
      }
      for (uint32_t v = 0; v < vertex_count; ++v) {
        float* const at = staged + size_t(v) * kFloatsPerVert;
        for (uint32_t k = 0; k < n; ++k) {
          std::memset(at + cleared[k], 0, 16);
        }
      }
    }
  }
  FillPhase(0, fill_mark);

  if (attrs.empty() || !memory) {
    if (snap.d3d_vertex_buffer != 0) {
      static std::mutex empty_mutex;
      static std::map<uint64_t, uint64_t> empty_by_shader;
      static std::atomic<uint64_t> last_ms{0};
      std::lock_guard lock(empty_mutex);
      ++empty_by_shader[snap.vs_hash];
      const uint64_t now = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      uint64_t was = last_ms.load(std::memory_order_relaxed);
      if (now - was >= 3000 && last_ms.compare_exchange_strong(was, now)) {
        std::string report;
        for (const auto& e : empty_by_shader) {
          report += fmt::format(" {:016X}x{}", e.first, e.second);
        }
        REXLOG_WARN("plume: scene draws with no known vertex layout:{}", report);
      }
    }
    for (uint32_t i = 0; i < vertex_count; ++i) {
      const uint32_t corner = i % 3;
      float* dst = vert(i);
      dst[0] = corner == 1 ? 3.0f : -1.0f;
      dst[1] = corner == 2 ? 3.0f : -1.0f;
      dst[2] = 0.0f;
      dst[3] = 1.0f;
    }
    // Built in the staging buffer like everything else, so it has to be copied
    // out like everything else - returning straight away drew whatever an
    // earlier frame had left at this place in the real buffer.
    CopyVerticesOut(vb_mapped_ + size_t(base_vertex) * kFloatsPerVert, staged, vertex_count);
    last_fill_passthrough_ = true;
    return vertex_count;
  }

  using rex::graphics::xenos::FetchConstantType;
  using rex::graphics::xenos::PrimitiveType;
  uint32_t fetched = 0;
  uint32_t fetch_addr = 0;
  uint32_t fetch_type = 0;
  // A draw that named its own buffers uses them. The vertex index comes from
  // the index buffer the title bound, so a strip or a list reaches the same
  // vertices it would have on the console; without that the run would be
  // expanded in order and the geometry would come apart.
  // Buffers arrive as addresses in the window the title uses, and that window
  // sits one page on from the plain physical address - the same offset the GPU
  // identifier block needed. Dropping the top bits alone lands a page short,
  // which reads a neighbouring buffer as though it were vertices.
  // The rule the title's Direct3D applies wherever it writes an address into a
  // packet: the 0xE0000000 and 0xF0000000 windows both sit a page ahead of
  // the physical address. Handling only 0xE put every buffer the race's
  // world keeps in 0xF a page short, and its vertices read as garbage.
  const auto physical_of = [](uint32_t guest) -> uint32_t {
    return (guest & 0x1FFFFFFFu) + (((guest >> 20) + 0x200u) & 0x1000u);
  };
  // Where a scene draw is being put. If the viewport transform is wrong the
  // geometry lands outside the frame and nothing is visible, with every other
  // part of the pipeline working perfectly - which is exactly what this looks
  // like from the outside.
  if (!passthrough && snap.num_indices >= 64) {
    static std::atomic<uint64_t> reported{0};
    const uint64_t n = reported.fetch_add(1, std::memory_order_relaxed);
    if (n < 6) {
      REXLOG_INFO(
          "plume: scene draw vs={:016X} indices={} viewport scale=({:.1f},{:.1f},{:.3f}) "
          "offset=({:.1f},{:.1f},{:.3f}) vte={:08X} depth={:08X}",
          snap.vs_hash, snap.num_indices, snap.vport_xscale, snap.vport_yscale,
          snap.vport_zscale, snap.vport_xoffset, snap.vport_yoffset, snap.vport_zoffset,
          snap.vte_cntl, snap.depth_control);
    }
  }

  if (pre) {
    if (!in_place) {
      std::memcpy(staged, pre_arena_.data() + pre->offset,
                  size_t(vertex_count) * kVertexStrideBytes);
    }
    if (cache_eligible) {
      read_lo = pre->read_lo;
      read_hi = pre->read_hi;
    }
    fetched = pre->fetched;
    fetch_addr = pre->fetch_addr;
    fetch_type = pre->fetch_type;
  } else {
    UnpackVertices(snap, attrs, pos_fetch_const, pos_float, vertex_count, memory, staged, read_lo, read_hi,
                   fetched, fetch_addr, fetch_type);
  }
  FillPhase(1, fill_mark);
  if (!attrs.empty() && pos_float + 4 <= kFloatsPerVert) {
    static const bool keep = std::getenv("XERENGE_KEEP_BAD_VERTICES") != nullptr;
    if (!keep) {
      if (const uint32_t bad = CollapseBadPrimitives(
              static_cast<rex::graphics::xenos::PrimitiveType>(snap.prim_type), staged, vertex_count, pos_float)) {
        static const bool trace = std::getenv("XERENGE_VERTEX_TRACE") != nullptr;
        static std::atomic<uint64_t> collapsed{0};
        static std::atomic<uint64_t> last_ms{0};
        const uint64_t total = collapsed.fetch_add(bad, std::memory_order_relaxed) + bad;
        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());
        uint64_t was = last_ms.load(std::memory_order_relaxed);
        if (trace && now - was >= 3000 && last_ms.compare_exchange_strong(was, now)) {
          REXLOG_INFO("plume: {} vertices with a bad position so far, their primitives dropped (last vs={:016X} "
                      "prim={} indices={})",
                      total, snap.vs_hash, snap.prim_type, snap.num_indices);
        }
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
  // An interface draw's extent, for the edge its object is moved to on a
  // screen wider than 16:9 (EncodeDraws).
  if (looks_pixel && snap.interface_object != 0 && vertex_count != 0) {
    float lo = vert(0)[0];
    float hi = lo;
    for (uint32_t vi = 1; vi < vertex_count; ++vi) {
      lo = std::min(lo, vert(vi)[0]);
      hi = std::max(hi, vert(vi)[0]);
    }
    last_fill_x_[0] = lo;
    last_fill_x_[1] = hi;
    last_fill_x_known_ = true;
  }

  if (passthrough && static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kRectangleList &&
      attrs.size() <= 2) {
    if (BindlessForTexture(snap) == 0) {
      // An untextured fullscreen quad is the menu's background dimmer. It
      // only painted over the UI while every draw was forced through one
      // fixed blend; with the guest's own blend state honoured it darkens
      // the scene as it should. A genuine opaque copy still has to go -
      // that one really would cover the screen.
      const uint32_t blend_control =
          snap.blend_control ? snap.blend_control : kDefaultBlendControl;
      if (GuestBlendIsOpaqueCopy(blend_control)) {
        return refuse("untextured opaque fullscreen quad");
      }
    }
    // Guest video PS samples Y+U+V in one draw. 640x360 chroma blits are
    // mid-gray and strobe over the luma frame. Do not skip font k_8_A glyphs.
    const auto fetch0 = TextureFetchAt(snap, 0);
    if (IsLumaFormat(fetch0.format) && fetch0.base_address != 0) {
      rex::graphics::TextureInfo info{};
      if (rex::graphics::TextureInfo::Prepare(fetch0, &info)) {
        const uint32_t width = info.width + 1;
        const uint32_t height = info.height + 1;
        if (width == 640 && height == 360) {
          return refuse("640x360 chroma blit");
        }
      }
    }
  }
  if (passthrough && looks_pixel) {
    float xscale = snap.vport_xscale;
    float xoffset = snap.vport_xoffset;
    float yscale = snap.vport_yscale;
    float yoffset = snap.vport_yoffset;
    // A Direct3D draw's viewport is its own target's, not whatever the ring
    // last programmed: after the garage the ring's registers still held a
    // 128-wide reflection face's, and the menu's video - positions in pixels
    // of its 1280x720 target - came out ten times too big, its top-left
    // corner filling the screen.
    static const bool targets_followed = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
    if (targets_followed && snap.d3d_vertex_buffer != 0 && snap.d3d_target_width != 0 &&
        snap.d3d_target_height != 0) {
      xscale = float(snap.d3d_target_width) * 0.5f;
      xoffset = xscale;
      yscale = -float(snap.d3d_target_height) * 0.5f;
      yoffset = -yscale;
    }
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
      // Half a pixel back. Direct3D 9 puts pixel centres on integer
      // coordinates and titles subtract half a pixel to compensate; Vulkan
      // puts them on half-integers, so that subtraction has to be undone.
      // Left in, it shifts everything half a pixel and leaves the right and
      // bottom edges of a fullscreen quad short - which showed up as a
      // one-pixel strip of the layer underneath along the screen edge.
      pos[0] = (pos[0] + 0.5f - xoffset) / xscale;
      pos[1] = (pos[1] + 0.5f - yoffset) / yscale;
      if (pos[3] == 0.0f) {
        pos[3] = 1.0f;
      }
    }
  }

  // A full-screen pass drawn with the title's own shaders: its vertex shader
  // hands the position straight on, in the render target's pixels, because
  // the pass runs with the viewport transform switched off. Nothing here
  // switches it off, so the pixels were taken as clip space and the quad
  // landed almost entirely off screen - the composite of the 3D frame came
  // out black. Put the position into clip space for it, against the size of
  // the target it was drawn into; the target is drawn over the whole frame
  // buffer, so normalised is what fits.
  // Only those passes: the interface also gives its positions in pixels, but
  // its vertex shader puts them into clip space itself, and doing it here as
  // well threw the whole interface off screen.
  if (!passthrough && ReadsResolvedCopy(snap, false) && pos_float + 4 <= kFloatsPerVert &&
      vertex_count != 0) {
    // What a pass over a copy is drawn with, when that changes: its target,
    // whether its copy is this frame's (and so gets converted here), the
    // first vertex and the vertex shader's first two constants - some of
    // these shaders scale positions themselves.
    static std::mutex seen_mutex;
    static std::set<std::tuple<uint64_t, uint32_t, uint32_t, bool, uint32_t, float>> seen;
    const float* pos = vert(0) + pos_float;
    const float* vc = reinterpret_cast<const float*>(snap.vs_constants.data());
    const bool this_frame = ReadsResolvedCopy(snap, true);
    const auto fetch = TextureFetchAt(snap, 0);
    std::lock_guard lock(seen_mutex);
    if (seen.size() < 200 &&
        seen.emplace(snap.vs_hash, snap.d3d_target_width, snap.d3d_target_height, this_frame,
                     fetch.base_address, vc[8] + vc[9] * 2.0f + vc[11] * 4.0f)
            .second) {
      REXLOG_INFO("plume: pass over copy {:08X} ({}x{}) vs={:016X} ps={:016X} into {}x{} "
                  "this frame {} scissor {} ({},{},{},{}) pos0 ({:.1f},{:.1f}) c0 ({:.4f},{:.4f},"
                  "{:.4f},{:.4f}) c1 ({:.4f},{:.4f},{:.4f},{:.4f}) c2 ({:.4f},{:.4f},{:.4f},{:.4f})",
                  fetch.base_address << 12, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                  snap.vs_hash, snap.ps_hash, snap.d3d_target_width, snap.d3d_target_height,
                  this_frame, snap.scissor_enabled, snap.scissor_rect[0], snap.scissor_rect[1],
                  snap.scissor_rect[2], snap.scissor_rect[3], pos[0], pos[1], vc[0], vc[1], vc[2],
                  vc[3], vc[4], vc[5], vc[6], vc[7], vc[8], vc[9], vc[10], vc[11]);
    }
  }
  const bool scales_itself = vs_info ? vs_info->is_position_scaling : false;
  if (!passthrough && !scales_itself && ReadsResolvedCopy(snap, true) &&
      snap.d3d_target_width != 0 && snap.d3d_target_height != 0 &&
      pos_float + 4 <= kFloatsPerVert) {
    bool in_pixels = false;
    for (uint32_t vi = 0; vi < vertex_count; ++vi) {
      const float* pos = vert(vi) + pos_float;
      if (std::fabs(pos[0]) > 2.0f || std::fabs(pos[1]) > 2.0f) {
        in_pixels = true;
        break;
      }
    }
    if (in_pixels) {
      const float w = float(snap.d3d_target_width);
      const float h = float(snap.d3d_target_height);
      for (uint32_t vi = 0; vi < vertex_count; ++vi) {
        float* pos = vert(vi) + pos_float;
        pos[0] = pos[0] / w * 2.0f - 1.0f;
        pos[1] = 1.0f - pos[1] / h * 2.0f;
      }
      static std::atomic<uint32_t> shown{0};
      if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
        REXLOG_INFO("plume: screen-space pass vs={:016X} into {}x{} - position at float {}, "
                    "vte {:08X}",
                    snap.vs_hash, snap.d3d_target_width, snap.d3d_target_height, pos_float,
                    snap.vte_cntl);
      }
    }
  }

  uint32_t draw_count = vertex_count;
  if (static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kRectangleList &&
      vertex_count >= 3) {
    draw_count = ExpandRectangle(staged, 0, vertex_count, pos_float);
  } else if (static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kQuadList &&
             vertex_count >= 4) {
    draw_count = ExpandQuadList(staged, 0, vertex_count);
  }

  if (passthrough) {
    for (uint32_t vi = 0; vi < draw_count; ++vi) {
      float* pos = vert(vi);
      pos[1] = -pos[1];
    }
    const float texid = float(BindlessForTexture(snap));
    const bool has_color_attr = HasColorAttr(attrs);
    float tint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int tint_slot = -1;
    int learned_slot = -1;
    if (auto it = ui_tint_slot_by_shader_.find(snap.vs_hash);
        it != ui_tint_slot_by_shader_.end()) {
      learned_slot = it->second;
    }
    const bool have_tint = ResolveUiTint(snap, has_color_attr, tint, &tint_slot, learned_slot);
    // Remember where an unambiguous colour was found so the same shader's
    // darker passes can read that slot directly next time.
    if (have_tint && learned_slot < 0 && tint_slot >= 0) {
      ui_tint_slot_by_shader_[snap.vs_hash] = tint_slot;
    }
    uint32_t tex_fmt = 0;
    uint32_t tex_w = 0;
    uint32_t tex_h = 0;
    for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
      if (BindlessForSlot(snap, slot) == 0) {
        continue;
      }
      const auto fetch = TextureFetchAt(snap, slot);
      tex_fmt = uint32_t(rex::graphics::GetBaseFormat(fetch.format));
      const uint64_t key = TextureKey(fetch);
      if (auto it = guest_textures_.find(key); it != guest_textures_.end() && it->second) {
        tex_w = it->second->width;
        tex_h = it->second->height;
      }
      break;
    }
    // XERENGE_VIDEO_GPU 3: a video frame drawn by the video pipeline carries its
    // planes' slots in this lane instead (video.vert): luma, Cb or packed
    // Cb,Cr, Cr, and 3 when packed.
    float lane[4] = {texid, 0.0f, 0.0f, 1.0f};
    if (UsesVideoPipeline(snap)) {
      const uint32_t u = VideoPlaneSlot(snap.video_u_key);
      lane[0] = float(VideoPlaneSlot(snap.video_key));
      lane[1] = float(u);
      lane[2] = float(snap.video_packed_uv ? u : VideoPlaneSlot(snap.video_v_key));
      lane[3] = snap.video_packed_uv ? 3.0f : 2.0f;
    }
    for (uint32_t vi = 0; vi < draw_count; ++vi) {
      float* dst = vert(vi);
      dst[4] = lane[0];
      dst[5] = lane[1];
      dst[6] = lane[2];
      dst[7] = lane[3];
      // 5B42 UI VS has no COLOR0 vfetch: guest writes oD0 from a VS constant.
      if (!has_color_attr) {
        if (have_tint) {
          dst[17 * 4 + 0] = tint[0];
          dst[17 * 4 + 1] = tint[1];
          dst[17 * 4 + 2] = tint[2];
          dst[17 * 4 + 3] = tint[3];
        } else {
          // Neither a real per-vertex colour nor a resolvable tint constant.
          // The vertex buffer was memset to 0 above, so without this the
          // draw would come out fully transparent black rather than the
          // harmless opaque white a title expects when it has not bothered
          // to drive colour at all - this used to be the vertex shader's
          // job (see passthrough.vert), before it could tell this case
          // apart from a real, legitimately dark vertex colour.
          dst[17 * 4 + 0] = 1.0f;
          dst[17 * 4 + 1] = 1.0f;
          dst[17 * 4 + 2] = 1.0f;
          dst[17 * 4 + 3] = 1.0f;
        }
      }
    }
    static uint32_t tint_logs = 0;
    if (tint_logs < 8 && have_tint) {
      ++tint_logs;
      REXLOG_INFO("plume: ui tint slot={} ({:.3f},{:.3f},{:.3f},{:.3f}) fmt={} {}x{} vs={:016X}",
                  tint_slot, tint[0], tint[1], tint[2], tint[3], tex_fmt, tex_w, tex_h,
                  snap.vs_hash);
    }
    // Diagnostic for the draws that carry no COLOR0 vfetch: these are the
    // ones whose colour has to come from a shader constant, and a pass whose
    // colour is black (a text drop shadow) is exactly what the tint heuristic
    // cannot tell apart from a matrix row. Dump what was actually available
    // so the rule can be based on real data rather than a guess.
    if (!has_color_attr) {
      static uint32_t nocolor_logs = 0;
      if (nocolor_logs < 16) {
        ++nocolor_logs;
        const float* c0f = reinterpret_cast<const float*>(snap.vs_constants.data());
        const float* ps0f = reinterpret_cast<const float*>(snap.ps_constants.data());
        REXLOG_INFO(
            "plume: nocolor vs={:016X} tint={} slot={} learned={} ({:.3f},{:.3f},{:.3f},{:.3f}) "
            "fmt={} {}x{} prim={} verts={}",
            snap.vs_hash, have_tint ? 1 : 0, tint_slot, learned_slot, tint[0], tint[1], tint[2],
            tint[3], tex_fmt, tex_w, tex_h, snap.prim_type, draw_count);
        REXLOG_INFO("plume:   ps0=({:.3f},{:.3f},{:.3f},{:.3f}) ps1=({:.3f},{:.3f},{:.3f},{:.3f})",
                    ps0f[0], ps0f[1], ps0f[2], ps0f[3], ps0f[4], ps0f[5], ps0f[6], ps0f[7]);
        for (uint32_t ci = 0; ci < 6; ++ci) {
          const float* c = c0f + ci * 4;
          REXLOG_INFO("plume:   vsc{}=({:.3f},{:.3f},{:.3f},{:.3f})", ci, c[0], c[1], c[2], c[3]);
        }
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
  // Horizontal extent of the near-fullscreen passthrough quads. A screen-edge
  // effect that leaves a one-pixel strip uncovered is a quad whose right edge
  // stops just short of 1.0, and the only way to tell that from a scissor or
  // sampling problem is to look at the number.
  if (passthrough && draw_count >= 3) {
    float min_x = vert(0)[0];
    float max_x = min_x;
    for (uint32_t vi = 1; vi < draw_count; ++vi) {
      const float x = vert(vi)[0];
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
    }
    if (max_x - min_x > 1.5f) {
      static uint32_t extent_logs = 0;
      if (extent_logs < 10) {
        ++extent_logs;
        REXLOG_INFO("plume: fullscreen quad x from {:.6f} to {:.6f} (tex {}) vs={:016X}", min_x,
                    max_x, BindlessForTexture(snap), snap.vs_hash);
      }
    }
  }
  (void)fetched;
  // Where an interface draw from the Direct3D calls actually lands. Such draws
  // reach the command list in the menus yet nothing appears, while the same
  // kind of draw shows in the car-select scene - so the placement is the
  // question: which path, which viewport, which final positions.
  if (snap.d3d_vertex_buffer != 0 && snap.d3d_index_buffer == 0 && draw_count >= 3) {
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 10) {
      const float* p0 = vert(0);
      const float* p1 = vert(1);
      const float* p2 = vert(2);
      REXLOG_INFO(
          "plume: interface draw vs={:016X} prim={} passthrough={} vport=({:.1f},{:.1f} / "
          "{:.1f},{:.1f}) depth={:08X} v0=({:.2f},{:.2f},{:.2f},{:.2f}) v1=({:.2f},{:.2f}) "
          "v2=({:.2f},{:.2f})",
          snap.vs_hash, snap.prim_type, passthrough, snap.vport_xscale, snap.vport_yscale,
          snap.vport_xoffset, snap.vport_yoffset, snap.depth_control, p0[0], p0[1], p0[2],
          p0[3], p1[0], p1[1], p2[0], p2[1]);
    }
  }
  // Never past the end of the real buffer. The expansions above guard their
  // own output, but they are handed the staging buffer at offset zero, so the
  // guard measures from the wrong place: a rectangle list drawn near the end
  // of a full frame expanded past it, the copy below wrote beyond the mapped
  // upload heap, the draw read beyond it, and the GPU hung. Keep whole
  // triangles only.
  if (base_vertex >= vb_limit_) {
    return 0;
  }
  const uint32_t room = vb_limit_ - base_vertex;
  if (draw_count > room) {
    static std::atomic<uint32_t> clipped{0};
    if (clipped.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_WARN("plume: draw of {} vertices cut to {} - vertex buffer full at {}", draw_count,
                  room - room % 3, base_vertex);
    }
    draw_count = room - room % 3;
    if (draw_count == 0) {
      return 0;
    }
  }
  FillPhase(2, fill_mark);
  // Kept from a key's second sighting on: a one-off draw from a buffer the
  // title refills every frame would fill the cache with garbage.
  if (cache_eligible && draw_count != 0 && mesh_cache_.count(cache_key) == 0 &&
      mesh_seen_once_.insert(cache_key).second) {
    cache_eligible = false;
  }
  uint32_t cache_offset = 0;
  if (cache_eligible && draw_count != 0) {
    // Taken out first: a key re-stored after its vertices changed must not
    // have its old place evicted as someone else's in the allocation below.
    if (auto old_entry = mesh_cache_.find(cache_key);
        old_entry != mesh_cache_.end() && old_entry->second.count != 0) {
      if (auto at = cache_by_offset_.find(old_entry->second.offset);
          at != cache_by_offset_.end() && at->second == cache_key) {
        cache_by_offset_.erase(at);
        retired_cache_regions_.push_back(
            {old_entry->second.offset, old_entry->second.count, old_entry->second.last_used});
      }
      // The entry gives up its place at once. It used to keep it until the
      // new one was stored, so when no room was found (or a read range had no
      // host page) it went on naming a place the retired list soon handed to
      // other meshes, and the vertices it had held earlier - a car's undamaged
      // shape again after a reset or a replay - hit it.
      old_entry->second.count = 0;
    }
    if (!AllocateCacheRegion(draw_count, &cache_offset)) {
      cache_eligible = false;
      static std::atomic<uint32_t> no_room{0};
      const uint32_t n = no_room.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4 || (n & (n - 1)) == 0) {
        REXLOG_INFO("plume: vertex cache: no room for {} vertices (head {}, {} meshes, {} retired places), "
                    "drawn uncached ({} so far)",
                    draw_count, cache_used_, mesh_cache_.size(), retired_cache_regions_.size(), n);
      }
    }
  }
  if (cache_eligible && draw_count != 0) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (size_t ai = 0; ai < read_lo.size(); ++ai) {
      if (read_lo[ai] <= read_hi[ai]) {
        ranges.emplace_back(read_lo[ai] << 2, (read_hi[ai] + 4) << 2);
      }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& r : ranges) {
      if (!merged.empty() && r.first <= merged.back().second) {
        merged.back().second = std::max(merged.back().second, r.second);
      } else {
        merged.push_back(r);
      }
    }
    uint64_t data_hash = 0;
    if (ranges_hash(merged, &data_hash)) {
      CachedMesh& entry = mesh_cache_[cache_key];
      if (entry.count != 0) {
        ++entry.changes;
      }
      entry.offset = cache_offset;
      entry.count = draw_count;
      entry.index_hash = index_hash;
      entry.data_hash = data_hash;
      entry.last_used = frame_serial_;
      entry.ranges = std::move(merged);
      cache_by_offset_[cache_offset] = cache_key;
      CopyVerticesOut(cache_mapped_ + size_t(cache_offset) * kFloatsPerVert, staged, draw_count);
      if (const uint32_t layout = fill_layout_mask_; layout != 0) {
        const uint32_t first_loc = uint32_t(std::countr_zero(layout));
        std::memcpy(entry.fingerprint.data(), staged + first_loc * 4, 16);
        entry.packed_floats = uint32_t(std::popcount(layout)) * 4;
        std::memcpy(entry.fingerprint.data() + 4,
                    staged + size_t(draw_count - 1) * kFloatsPerVert + first_loc * 4, 16);
      }
      last_fill_cache_offset_ = cache_offset;
      FillPhase(3, fill_mark);
      return draw_count;
    }
  }
  // The one touch of the upload heap: a single sequential copy, which write
  // combined memory handles at full speed.
  CopyVerticesOut(vb_mapped_ + size_t(base_vertex) * kFloatsPerVert, staged, draw_count);
  FillPhase(3, fill_mark);
  return draw_count;
}

// Debug mode: each pass shape once - where it starts, what it leaves
// unloaded and unstored.
void LogPassAccess(size_t from, uint32_t area_w, uint32_t no_load, uint32_t discard) {
  static std::mutex seen_mutex;
  static std::set<std::tuple<size_t, uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard lock(seen_mutex);
  if (seen.size() < 64 && seen.emplace(from, area_w, no_load, discard).second) {
    REXLOG_INFO("plume: pass from entry {} ({} wide): not loaded {:X}, not stored {:X} "
                "(1 colour, 2 second target, 80000000 depth)",
                from, area_w, no_load, discard);
  }
}

void ClearFirstPass(plume::RenderCommandList* list, const RenderPassBreak* pass,
                    uint32_t no_load, uint32_t discard) {
  if (!list || !pass || !pass->clear_first) {
    return;
  }
  list->setAttachmentAccess(no_load, discard);
  list->clearColor(0, plume::RenderColor(pass->clear_color[0], pass->clear_color[1],
                                         pass->clear_color[2], pass->clear_color[3]));
  if (pass->second_target) {
    list->clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  if (pass->has_depth) {
    // Far plane, matching the guest's own convention of clearing depth to 1.
    list->clearDepth(true, 1.0f);
  }
}

bool PlumeSecondTargetEnabled() {
  // XERENGE_NO_SECOND_TARGET: no render target 1 (motion vectors, read only by
  // the motion blur) - on a phone's tiled GPU every pixel was written twice.
  // Nor with motion blur off (XERENGE_NO_MOTION_BLUR): the patch for it leaves
  // the blur at speed, which still drew from the vectors; without them the
  // setting does what it says, and the scene pass carries one colour target.
  static const bool on = std::getenv("XERENGE_D3D_TARGETS") != nullptr &&
                         std::getenv("XERENGE_NO_SECOND_TARGET") == nullptr &&
                         std::getenv("XERENGE_NO_MOTION_BLUR") == nullptr;
  return on;
}

void PlumeDrawContext::UploadLayeredTexture(plume::RenderCommandList* list, uint64_t key,
                                            const rex::graphics::TextureInfo& info,
                                            const uint8_t* src, plume::RenderFormat host_format,
                                            bool expand_r8, bool swap_bgra) {
  const rex::graphics::FormatInfo* fi = info.format_info();
  if (!fi || fi->block_width != 1 || fi->block_height != 1 || !device_ || !volume_set_) {
    return;
  }
  const uint32_t width = info.width + 1;
  const uint32_t height = info.height + 1;
  // TextureInfo keeps the depth as the fetch constant does, one less than the
  // count - the colour grading table came up a slice short (31 of 32), its
  // brightest slice missing and every other one stretched to fill.
  const uint32_t layers = std::min<uint32_t>(info.depth + 1, 256);
  const uint32_t bpb = fi->bytes_per_block();
  // Each slice is a whole 2D image, one after the other, padded to the tiled
  // (or pitched) extent.
  const size_t slice_bytes =
      size_t(info.extent.block_pitch_h) * info.extent.block_pitch_v * bpb;
  if (slice_bytes == 0 || layers < 2) {
    return;
  }
  const uint64_t content = XXH3_64bits(src, slice_bytes * layers);
  LayeredTexture& tex = layered_textures_[key];
  if (tex.texture && tex.content == content) {
    return;
  }
  std::vector<uint8_t> all;
  uint32_t row_texels = 0;
  uint32_t row_bytes = 0;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    std::vector<uint8_t> pixels;
    uint32_t rb = 0;
    uint32_t rt = 0;
    if (!UntileGuestTexture(info, src + slice_bytes * layer, &pixels, &rb, &rt, expand_r8) ||
        pixels.empty()) {
      return;
    }
    if (swap_bgra) {
      SwizzleBgraToRgba(&pixels, height, rb);
    }
    row_bytes = rb;
    row_texels = rt;
    all.insert(all.end(), pixels.begin(), pixels.begin() + size_t(rb) * height);
  }
  if (!tex.texture || tex.width != width || tex.height != height || tex.layers != layers) {
    if (tex.index == 0) {
      if (next_layered_index_ >= kBindlessTextureCount) {
        return;
      }
      tex.index = next_layered_index_++;
    }
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture(
        plume::RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, layers, host_format);
    desc.committed = true;
    tex.texture = device_->createTexture(desc);
    if (!tex.texture) {
      return;
    }
    tex.view = tex.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(host_format));
    if (!tex.view) {
      tex.texture.reset();
      return;
    }
    tex.width = width;
    tex.height = height;
    tex.layers = layers;
    REXLOG_INFO("plume: layered texture {:08X} {}x{}x{} as 2D array (volume index {})",
                info.memory.base_address, width, height, layers, tex.index);
  }
  // This frame's staging buffer; the last frame's may still be copied from.
  const uint32_t half = uint32_t(frame_serial_ & 1);
  auto& staging = tex.staging[half];
  if (!staging || tex.staging_size[half] < all.size()) {
    staging = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(all.size()));
    if (!staging) {
      tex.staging_size[half] = 0;
      return;
    }
    tex.staging_size[half] = all.size();
  }
  if (void* mapped = staging->map()) {
    std::memcpy(mapped, all.data(), all.size());
    staging->unmap();
  } else {
    return;
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(tex.texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t layer = 0; layer < layers; ++layer) {
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(tex.texture.get(), 0, layer),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging.get(), host_format, width, height, 1, row_texels,
            uint64_t(row_bytes) * height * layer),
        0, 0, 0, nullptr);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(tex.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  volume_set_->setTexture(tex.index, tex.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          tex.view.get());
  tex.content = content;
}

uint32_t PlumeDrawContext::SamplerKey(const GuestDrawSnapshot& snap, uint32_t slot) const {
  const auto fetch = TextureFetchAt(snap, slot);
  return uint32_t(fetch.clamp_x) | (uint32_t(fetch.clamp_y) << 3) |
         (uint32_t(fetch.clamp_z) << 6) | (uint32_t(fetch.mag_filter) << 9) |
         (uint32_t(fetch.min_filter) << 11) | (uint32_t(fetch.mip_filter) << 13) |
         (uint32_t(fetch.aniso_filter) << 15) | ((uint32_t(fetch.lod_bias) & 0x3FFu) << 18);
}

void PlumeDrawContext::EnsureSampler(const GuestDrawSnapshot& snap, uint32_t slot) {
  if (!device_ || !sampler_set_) {
    return;
  }
  const uint32_t key = SamplerKey(snap, slot);
  if (sampler_index_by_key_.count(key) != 0 || next_sampler_ >= kBindlessSamplerCount) {
    return;
  }
  using rex::graphics::xenos::ClampMode;
  auto address = [](uint32_t mode) {
    switch (static_cast<ClampMode>(mode)) {
      case ClampMode::kRepeat:
        return plume::RenderTextureAddressMode::WRAP;
      case ClampMode::kMirroredRepeat:
        return plume::RenderTextureAddressMode::MIRROR;
      case ClampMode::kMirrorClampToEdge:
      case ClampMode::kMirrorClampToHalfway:
      case ClampMode::kMirrorClampToBorder:
        return plume::RenderTextureAddressMode::MIRROR_ONCE;
      case ClampMode::kClampToBorder:
        return plume::RenderTextureAddressMode::BORDER;
      default:
        return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  // Point is 0 in TextureFilter; everything else - linear, base map, "as the
  // fetch constant says" - filters linearly.
  auto filter = [](uint32_t f) {
    return f == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  };
  plume::RenderSamplerDesc desc;
  desc.addressU = address(key & 7u);
  desc.addressV = address((key >> 3) & 7u);
  desc.addressW = address((key >> 6) & 7u);
  desc.magFilter = filter((key >> 9) & 3u);
  desc.minFilter = filter((key >> 11) & 3u);
  // Between mips as the fetch says: point, linear, or the base alone.
  using rex::graphics::xenos::TextureFilter;
  const auto mip_filter = TextureFilter((key >> 13) & 3u);
  desc.mipmapMode = mip_filter == TextureFilter::kPoint ? plume::RenderMipmapMode::NEAREST
                                                        : plume::RenderMipmapMode::LINEAR;
  desc.maxLOD = mip_filter == TextureFilter::kBaseMap ? 0.0f : 16.0f;
  // Anisotropy as the fetch asks for it (kMax_1_1 .. kMax_16_1); the road
  // seen along its length needs it to stay sharp once it has mips.
  const uint32_t aniso = (key >> 15) & 7u;
  // Capped by XERENGE_MAX_ANISOTROPY (1: none) - at 4 on phones by default:
  // the title asks 8 of the road and the scene, which a phone's GPU pays for
  // in every pixel of them.
  static const uint32_t max_anisotropy = [] {
    const char* v = std::getenv("XERENGE_MAX_ANISOTROPY");
#ifdef __ANDROID__
    const unsigned long fallback = 4;
#else
    const unsigned long fallback = 16;
#endif
    const unsigned long n = v && *v ? std::strtoul(v, nullptr, 10) : fallback;
    return uint32_t(std::clamp<unsigned long>(n, 1, 16));
  }();
  if (aniso >= 2 && aniso <= 5) {
    desc.maxAnisotropy = std::min(1u << (aniso - 1), max_anisotropy);
    desc.anisotropyEnabled = desc.maxAnisotropy > 1;
  }
  // 5 fractional bits, signed.
  desc.mipLODBias = float(int32_t((key >> 18) & 0x3FFu) << 22 >> 22) / 32.0f;
  desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
  auto sampler = device_->createSampler(desc);
  if (!sampler) {
    return;
  }
  const uint32_t index = next_sampler_++;
  sampler_set_->setSampler(index, sampler.get());
  guest_samplers_.push_back(std::move(sampler));
  sampler_index_by_key_[key] = index;
}

uint32_t PlumeDrawContext::AcquireTextureSlot() {
  if (next_bindless_ < kBindlessTextureCount) {
    return next_bindless_++;
  }
  // Every slot taken. The race streams its textures in as it goes, each at a
  // new address, and with nothing ever given back the slots ran out mid-race:
  // new textures failed to appear and old ones showed in their place.
  auto stalest = guest_textures_.end();
  for (auto it = guest_textures_.begin(); it != guest_textures_.end(); ++it) {
    if (!it->second || it->second->last_used + 3 > frame_serial_) {
      continue;
    }
    if (stalest == guest_textures_.end() || it->second->last_used < stalest->second->last_used) {
      stalest = it;
    }
  }
  if (stalest == guest_textures_.end()) {
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_WARN("plume: no texture slot free and none stale enough to evict");
    }
    return 0;
  }
  const uint32_t slot = stalest->second->bindless;
  static std::atomic<uint64_t> evictions{0};
  const uint64_t n = evictions.fetch_add(1, std::memory_order_relaxed);
  if (n < 4 || (n % 1000) == 0) {
    REXLOG_INFO("plume: texture slots full - evicting {:016X} (unused {} frames), {} evictions",
                stalest->first, frame_serial_ - stalest->second->last_used, n + 1);
  }
  // Back to the empty texture before the new one takes the slot: a texture made
  // at the freed one's address must not look like the same entry (DrawSetFor).
  texture_set_->setTexture(slot, null_texture_.get(), plume::RenderTextureLayout::SHADER_READ,
                           null_texture_view_.get());
  guest_textures_.erase(stalest);
  return slot;
}

bool PlumeDrawContext::PixelShaderWritesOc1(uint64_t ps_hash) {
  if (auto it = ps_writes_oc1_.find(ps_hash); it != ps_writes_oc1_.end()) {
    return it->second;
  }
  const ShaderSourceInfo* info = FindShaderSourceInfo(ps_hash);
  const bool writes = info && info->writes_oc1;
  ps_writes_oc1_[ps_hash] = writes;
  return writes;
}

uint32_t PlumeDrawContext::ShaderSamplerSlots(uint64_t hash) const {
  if (auto it = sampler_slots_by_shader_.find(hash); it != sampler_slots_by_shader_.end()) {
    return it->second;
  }
  // Every slot, for a shader whose source was never seen.
  const ShaderSourceInfo* info = FindShaderSourceInfo(hash);
  const uint32_t mask = info ? info->sampler_slots : ~0u;
  sampler_slots_by_shader_[hash] = mask;
  return mask;
}

uint32_t PlumeDrawContext::UsedTextureSlots(const GuestDrawSnapshot& snap) const {
  // Only a Direct3D draw with the title's own shaders: the passthrough path
  // and the video take whichever slot is bound.
  if (snap.d3d_vertex_buffer == 0 || snap.has_video_frame() || snap.ps_hash == 0) {
    return ~0u;
  }
  const uint32_t ps_slots = ShaderSamplerSlots(snap.ps_hash);
  if (ps_slots == ~0u) {
    return ~0u;
  }
  uint32_t used = ps_slots & 0xFFFFu;
  const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
  const bool vertex_fetch = vs_info ? vs_info->is_vertex_fetch : false;
  if (vertex_fetch) {
    const uint32_t vs_slots = ShaderSamplerSlots(snap.vs_hash);
    if (vs_slots == ~0u) {
      return ~0u;
    }
    used |= (vs_slots & 0xFu) << 16;
  }
  return used;
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
  if (null_cube_) {
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(null_cube_.get(),
                                               plume::RenderTextureLayout::COPY_DEST));
    for (uint32_t face = 0; face < 6; ++face) {
      const plume::RenderTextureCopyLocation cube_src =
          plume::RenderTextureCopyLocation::PlacedFootprint(
              null_texture_staging_.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
      list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(null_cube_.get(), 0, face), cube_src, 0,
          0, 0, nullptr);
    }
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(null_cube_.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
  }
  if (identity_lut_ && identity_lut_staging_) {
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(identity_lut_.get(),
                                               plume::RenderTextureLayout::COPY_DEST));
    const uint32_t n = kIdentityLutSize;
    for (uint32_t layer = 0; layer < n; ++layer) {
      const plume::RenderTextureCopyLocation lut_src =
          plume::RenderTextureCopyLocation::PlacedFootprint(
              identity_lut_staging_.get(), plume::RenderFormat::R8G8B8A8_UNORM, n, n, 1, n,
              uint64_t(layer) * n * n * 4);
      const plume::RenderTextureCopyLocation lut_dst =
          plume::RenderTextureCopyLocation::Subresource(identity_lut_.get(), 0, layer);
      list->copyTextureRegion(lut_dst, lut_src, 0, 0, 0, nullptr);
    }
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(identity_lut_.get(),
                                               plume::RenderTextureLayout::SHADER_READ));
  }
  null_texture_uploaded_ = true;
}

bool PlumeDrawContext::UploadHostTexture(plume::RenderCommandList* list, uint64_t key,
                                         uint32_t width, uint32_t height,
                                         plume::RenderFormat host_format,
                                         const std::vector<uint8_t>& pixels, uint32_t row_texels,
                                         const std::vector<HostMipLevel>* mips) {
  if (!list || !device_ || !texture_set_ || pixels.empty() || width == 0 || height == 0 ||
      row_texels == 0) {
    return false;
  }
  const uint32_t levels = 1 + (mips ? uint32_t(mips->size()) : 0u);
  // Every level goes through one staging buffer, each at an offset any copy
  // accepts (a multiple of the block size and of 512 for D3D12).
  std::vector<uint64_t> level_offsets(levels, 0);
  uint64_t staging_needed = pixels.size();
  for (uint32_t level = 1; level < levels; ++level) {
    level_offsets[level] = rex::align<uint64_t>(staging_needed, 512);
    staging_needed = level_offsets[level] + (*mips)[level - 1].pixels.size();
  }
  GuestHostTexture* host = nullptr;
  std::unique_ptr<GuestHostTexture> created;
  if (auto it = guest_textures_.find(key); it != guest_textures_.end() && it->second) {
    host = it->second.get();
    if (host->width != width || host->height != height || host->format != host_format ||
        host->levels != levels) {
      return false;
    }
  } else {
    const uint32_t slot = AcquireTextureSlot();
    if (slot == 0) {
      return false;
    }
    created = std::make_unique<GuestHostTexture>();
    created->key = key;
    created->bindless = slot;
    created->last_used = frame_serial_;
    created->width = width;
    created->height = height;
    created->format = host_format;
    created->levels = levels;
    plume::RenderTextureDesc tex_desc =
        plume::RenderTextureDesc::Texture2D(width, height, levels, host_format);
    tex_desc.committed = true;
    created->texture = device_->createTexture(tex_desc);
    if (!created->texture || !VulkanTextureOk(created->texture.get())) {
      return false;
    }
    created->view =
        created->texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(host_format));
    if (!created->view) {
      return false;
    }
    host = created.get();
  }

  // Reuse the staging buffer when one of sufficient size is already here. A
  // texture that is genuinely re-uploaded every frame - a video plane - would
  // otherwise allocate a fresh upload buffer every frame.
  // This frame's staging buffer: the last frame's may still be copied from.
  const uint32_t half = uint32_t(frame_serial_ & 1);
  auto& staging = host->staging[half];
  if (!staging || host->staging_size[half] < staging_needed) {
    staging = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(staging_needed));
    if (!staging) {
      host->staging_size[half] = 0;
      return false;
    }
    host->staging_size[half] = staging_needed;
  }
  if (auto* mapped = static_cast<uint8_t*>(staging->map())) {
    std::memcpy(mapped, pixels.data(), pixels.size());
    for (uint32_t level = 1; level < levels; ++level) {
      const auto& mip = (*mips)[level - 1];
      std::memcpy(mapped + level_offsets[level], mip.pixels.data(), mip.pixels.size());
    }
    staging->unmap();
  }

  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(host->texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t level_width = level ? (*mips)[level - 1].width : width;
    const uint32_t level_height = level ? (*mips)[level - 1].height : height;
    const uint32_t level_row = level ? (*mips)[level - 1].row_texels : row_texels;
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(host->texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), host_format,
                                                          level_width, level_height, 1, level_row,
                                                          level_offsets[level]),
        0, 0, 0, nullptr);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(host->texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  texture_set_->setTexture(host->bindless, host->texture.get(),
                           plume::RenderTextureLayout::SHADER_READ, host->view.get());
  host->uploaded = true;
  if (created) {
    guest_textures_.emplace(key, std::move(created));
  }
  return true;
}

bool PlumeDrawContext::HoldsThinFrame(const std::vector<GuestDrawSnapshot>& draws) {
  uint32_t d3d = 0;
  uint32_t copies = 0;
  for (const auto& d : draws) {
    copies += d.is_resolve ? 1 : 0;
    d3d += (!d.is_clear && !d.is_resolve && d.d3d_vertex_buffer != 0) ? 1 : 0;
  }
  // 8, not 50: the title screen draws some 45 a frame over its video, and on a
  // phone its bare video frames came through (the logo and frame blinking out).
  // Video alone averages one draw and is still shown.
  const bool hold = d3d == 0 && copies == 0 && hold_average_ >= 8.0;
  hold_average_ = hold_average_ * 0.9 + double(d3d) * 0.1;
  if (hold) {
    static std::atomic<uint32_t> held{0};
    const uint32_t n = held.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n % 500) == 0) {
      REXLOG_INFO("plume: frame {} has {} entries and no Direct3D draw; showing the last picture again ({} so far)",
                  frame_serial_, draws.size(), n);
    }
  }
  return hold;
}

void PlumeDrawContext::PresentResolvedFrame(plume::RenderCommandList* list,
                                            plume::RenderTexture* color, uint32_t width,
                                            uint32_t height, uint32_t front_buffer) {
  // The front buffer the title swaps to wins; the frame's last copy is only a
  // fallback. A 3D frame copies into the front buffer, clears, and then makes
  // further copies of the cleared target - the last copy is black.
  //
  // And only when that copy was made in this frame. The menus resolve into the
  // front buffer and then clear the target, so the copy is the picture. A 3D
  // frame never calls Resolve for it - Swap resolves on its own - so the
  // target as it stands is the picture, and an older copy is stale.
  //
  // Which copy that is comes from the frame itself: the one nothing was drawn
  // over afterwards (EncodeDraws leaves it in frame_output_dest_). Matching it
  // against the front buffer named in the ring's swap packet went wrong
  // whenever that packet belonged to the neighbouring frame - the copy was
  // passed over and the target, already cleared for the next frame, was
  // shown: the interface blinked out to black.
  // Failing that, the old rule: the front buffer named by the swap, when the
  // frame copied into it.
  if (front_buffer != 0) {
    known_front_buffers_.insert(front_buffer);
  }
  if (frame_output_dest_ == 0 && front_buffer != 0 &&
      frame_resolved_dests_.count(front_buffer) != 0) {
    frame_output_dest_ = front_buffer;
  }
  // A frame that drew nothing and copied nothing - a clear and a Swap, or tiny spark effects
  // after a whole clear without resolve. A console shows its front buffer unchanged then;
  // shown from the target it was a black frame. Show the last picture again.
  // And a frame of a few draws and no copy of its own while the frames before
  // were shown from their copies (the menus: drawn, copied to one of two front
  // buffers, shown from the copy) - on a slow device every sixth or so held
  // only a clear and the background video, and the menu blinked out.
  const bool empty_or_cleared_secondary =
      (frame_encoded_draws_ == 0) ||
      (frame_cleared_whole_ && frame_encoded_draws_ <= 8) ||
      (!frame_drawn_since_copy_ && frame_cleared_whole_) ||
      (last_shown_from_copy_ && frame_encoded_draws_ <= 8);
  if (frame_output_dest_ == 0 && empty_or_cleared_secondary) {
    const uint32_t fallback = last_output_dest_ != 0 ? last_output_dest_ : front_buffer;
    if (fallback != 0 && resolved_targets_.find(fallback) != resolved_targets_.end()) {
      frame_output_dest_ = fallback;
      static std::atomic<uint32_t> repeated{0};
      const uint32_t n = repeated.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n % 120) == 0) {
        REXLOG_INFO("plume: frame {} drew {} draws; showing the last picture {:08X} again ({} so far)",
                    frame_serial_, frame_encoded_draws_, frame_output_dest_, n + 1);
      }
    }
  }
  if (frame_output_dest_ != 0) {
    last_output_dest_ = frame_output_dest_;
  }
  last_shown_from_copy_ = frame_output_dest_ != 0;
  static const bool targets = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  {
    static std::atomic<uint32_t> shown{0};
    if (targets && shown.fetch_add(1, std::memory_order_relaxed) < 12) {
      const auto found = resolved_targets_.find(frame_output_dest_);
      REXLOG_INFO("plume: present source {:08X} ({})", frame_output_dest_,
                  frame_output_dest_ == 0            ? "no copy this frame"
                  : found == resolved_targets_.end() ? "not among the copies"
                                                     : "found");
    }
  }
  if (!targets || !list || !color || frame_output_dest_ == 0) {
    return;
  }
  const auto it = resolved_targets_.find(frame_output_dest_);
  if (it == resolved_targets_.end() || !it->second.texture) {
    return;
  }
  // Only a copy of the whole frame: a post-processing buffer or a cube face
  // is no picture. The frame's own size, not the console's - drawn at 540p
  // every copy was refused and the screen alternated with the bare target.
  if (it->second.cube || it->second.width != width || it->second.height != height) {
    REXLOG_WARN("plume: refusing to present invalid/incompatible target {:08X} ({}x{}, cube={})",
                frame_output_dest_, it->second.width, it->second.height, it->second.cube);
    return;
  }
  plume::RenderTexture* frame = it->second.texture.get();
  const plume::RenderTextureBarrier to_copy[2] = {
      plume::RenderTextureBarrier(frame, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COPY_DEST)};
  list->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, to_copy, 2);
  list->copyTexture(color, frame);
  const plume::RenderTextureBarrier after[2] = {
      plume::RenderTextureBarrier(frame, plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE)};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, after, 2);
}

void PlumeDrawContext::ResolveRenderTarget(plume::RenderCommandList* list,
                                           plume::RenderTexture* color, uint32_t width,
                                           uint32_t height, uint32_t dest_base,
                                           uint32_t dest_width, uint32_t dest_height,
                                           uint32_t region_width, uint32_t region_height,
                                           uint32_t face, bool cube, bool zero) {
  if (!list || !color || !device_ || !texture_set_ || dest_base == 0 || width == 0 || height == 0) {
    return;
  }
  // Kept at the size actually rendered, which is the host's resolution rather
  // than the guest's. The guest samples a resolve target with normalised
  // coordinates, so its own idea of the size (dest_width/dest_height, from
  // RB_COPY_DEST_PITCH) does not have to be honoured - and matching it would
  // mean throwing away the resolution the scene was just rendered at, or
  // cropping it to a corner. Its extent is still worth knowing: it says what
  // aspect ratio the guest composed for.
  const uint32_t guest_width = dest_width;
  const uint32_t guest_height = dest_height;
  dest_width = width;
  dest_height = height;
  // A target drawn at its own size occupies only a corner; that corner is the
  // copy. A cube face is square and lands in its face of a cube map.
  const bool region = region_width != 0 && region_height != 0 &&
                      (region_width < width || region_height < height);
  if (region) {
    dest_width = region_width;
    dest_height = region_height;
  }
  cube = cube && cube_set_ && face < 6;
  if (cube) {
    dest_width = dest_height = std::min(dest_width, dest_height);
  }

  ResolvedTarget& target = resolved_targets_[dest_base];
  if (!target.texture || target.width != dest_width || target.height != dest_height ||
      target.cube != cube) {
    if (target.bindless == 0) {
      if (next_bindless_ >= kBindlessTextureCount) {
        return;
      }
      target.bindless = next_bindless_++;
    }
    // Same format as the colour attachment, so the copy needs no conversion.
    plume::RenderTextureDesc desc =
        cube ? plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D,
                                                 dest_width, dest_height, 1, 1, 6,
                                                 kColorTargetFormat,
                                                 plume::RenderTextureFlag::CUBE)
             : plume::RenderTextureDesc::Texture2D(dest_width, dest_height, 1,
                                                   kColorTargetFormat);
    desc.committed = true;
    target.texture = device_->createTexture(desc);
    if (!target.texture) {
      return;
    }
    target.view = target.texture->createTextureView(
        cube ? plume::RenderTextureViewDesc::TextureCube(kColorTargetFormat)
             : plume::RenderTextureViewDesc::Texture2D(kColorTargetFormat));
    target.cube = cube;
    if (!target.view) {
      target.texture.reset();
      return;
    }
    target.width = dest_width;
    target.height = dest_height;
    target.zeroed = false;
    REXLOG_INFO("plume: resolve target {:08X} -> {}x{} native (guest asked {}x{}, bindless {})",
                dest_base, dest_width, dest_height, guest_width, guest_height, target.bindless);
  }

  if (zero) {
    if (!target.zeroed && !target.cube) {
      const uint64_t bytes = uint64_t(dest_width) * dest_height * 4;
      if (!zero_upload_ || zero_upload_bytes_ < bytes) {
        zero_upload_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
        void* mapped = zero_upload_ ? zero_upload_->map() : nullptr;
        if (!mapped) {
          zero_upload_.reset();
          return;
        }
        // "No motion" as the scene's shaders write it into render target 1:
        // x = min(vx + 1, 1), y = max(vx, 0), z and w the same for vy, so
        // (1, 0, 1, 0), and the blur decodes x + y - 1. Zeroes read as a motion
        // of -1 on both axes: the blur's eight taps went off the picture and
        // the takedown camera, which always blurs, showed one flat colour. Red
        // and blue are both 255, so the bytes are the same in BGRA and RGBA.
        auto* texels = static_cast<uint32_t*>(mapped);
        const uint8_t still[4] = {0xFF, 0x00, 0xFF, 0x00};
        uint32_t pattern = 0;
        std::memcpy(&pattern, still, sizeof(pattern));
        std::fill(texels, texels + bytes / 4, pattern);
        zero_upload_->unmap();
        zero_upload_bytes_ = bytes;
      }
      list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(target.texture.get(),
                                                 plume::RenderTextureLayout::COPY_DEST));
      list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(target.texture.get(), 0, 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(zero_upload_.get(), kColorTargetFormat,
                                                            dest_width, dest_height, 1,
                                                            dest_width, 0));
      list->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(target.texture.get(),
                                                 plume::RenderTextureLayout::SHADER_READ));
      texture_set_->setTexture(target.bindless, target.texture.get(),
                               plume::RenderTextureLayout::SHADER_READ, target.view.get());
      target.zeroed = true;
    }
    return;
  }

  // The colour attachment is mid-render here, so both sides have to be moved
  // into copy layouts and then back - the destination to being sampleable.
  const plume::RenderTextureBarrier to_copy[2] = {
      plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(target.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
  list->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, to_copy, 2);
  // XERENGE_FRAME_PROBE: the middle of what this copies from, as drawn - read
  // back four frames on, by when the GPU is done with it.
  static const bool probe = std::getenv("XERENGE_FRAME_PROBE") != nullptr;
  if (probe && !region && !cube && width >= 64 && height >= 64) {
    if (resolve_probe_pending_ && frame_serial_ >= resolve_probe_frame_ + 4) {
      resolve_probe_pending_ = false;
      const auto* px = static_cast<const uint8_t*>(resolve_probe_mapped_);
      uint64_t sum = 0;
      for (uint32_t i = 0; i < 64 * 64; ++i) {
        sum += uint32_t(px[i * 4]) + px[i * 4 + 1] + px[i * 4 + 2];
      }
      static uint32_t shown = 0;
      if (shown++ < 12 || (shown % 120) == 0) {
        REXLOG_INFO("plume: resolve probe - middle of the copied target {}x{} brightness {:.3f}",
                    width, height, double(sum) / (64.0 * 64.0 * 3.0 * 255.0));
      }
    }
    if (!resolve_probe_pending_) {
      if (!resolve_probe_) {
        resolve_probe_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(64 * 64 * 4));
        resolve_probe_mapped_ = resolve_probe_ ? resolve_probe_->map() : nullptr;
      }
      if (resolve_probe_mapped_) {
        const plume::RenderBox box(int32_t(width / 2 - 32), int32_t(height / 2 - 32),
                                   int32_t(width / 2 + 32), int32_t(height / 2 + 32));
        list->copyTextureRegion(
            plume::RenderTextureCopyLocation::PlacedFootprint(resolve_probe_.get(), kColorTargetFormat, 64,
                                                              64, 1, 64, 0),
            plume::RenderTextureCopyLocation::Subresource(color, 0, 0), 0, 0, 0, &box);
        resolve_probe_pending_ = true;
        resolve_probe_frame_ = frame_serial_;
      }
    }
  }
  // The whole frame, one to one: source and destination are the same size, so
  // nothing is cropped and nothing is rescaled. A target drawn at its own size
  // copies its corner instead, into its face when it is a cube map.
  if (region || cube) {
    const plume::RenderBox box(0, 0, int32_t(dest_width), int32_t(dest_height));
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(target.texture.get(), 0, cube ? face : 0),
        plume::RenderTextureCopyLocation::Subresource(color, 0, 0), 0, 0, 0, &box);
  } else {
    list->copyTexture(target.texture.get(), color);
  }
  const plume::RenderTextureBarrier after_copy[2] = {
      plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(target.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, after_copy, 2);
  if (cube) {
    cube_set_->setTexture(target.bindless, target.texture.get(),
                          plume::RenderTextureLayout::SHADER_READ, target.view.get());
  } else {
    texture_set_->setTexture(target.bindless, target.texture.get(),
                             plume::RenderTextureLayout::SHADER_READ, target.view.get());
  }
}

bool PlumeDrawContext::ZeroedTargetReady(uint32_t dest_base, uint32_t width, uint32_t height,
                                         uint32_t region_width, uint32_t region_height) const {
  const auto it = resolved_targets_.find(dest_base);
  if (it == resolved_targets_.end() || !it->second.texture || !it->second.zeroed ||
      it->second.cube) {
    return false;
  }
  const bool region = region_width != 0 && region_height != 0 &&
                      (region_width < width || region_height < height);
  return it->second.width == (region ? region_width : width) &&
         it->second.height == (region ? region_height : height);
}

namespace {
void EncodeStage(const char* stage, const GuestDrawSnapshot* snap = nullptr);
}  // namespace

// The vertex writer trap (XERENGE_VERTEX_TRACE): when an absurd vertex attribute is found in guest memory, the
// pages around it are watched, and each write to them logs the host stack of the writer - the recompiled code runs
// as native functions named after their guest address, so the stack says which guest routine wrote the vertices.
// Frames are "module+offset" (nm the build's binary for the sub_XXXXXXXX). The pages are watched again every frame.
namespace {
struct VertexTrap {
  std::mutex mutex;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;  // physical start, length
  std::vector<const char*> kinds;                      // "vertices" or "constants", one per range
  std::map<uint64_t, uint32_t> writers;                // stack signature -> writes
  std::atomic<bool> armed{false};
  std::atomic<uint32_t> events{0};
  uint32_t absurd_seen = 0;
};
VertexTrap& Trap() {
  static VertexTrap trap;
  return trap;
}

// An absurd value was read at this physical byte address: watch its neighbourhood from the next frame on.
void TrapArm(uint32_t physical_byte_address) {
  auto& trap = Trap();
  std::lock_guard lock(trap.mutex);
  if (trap.absurd_seen++ < 40) {
    REXLOG_INFO("plume: vertex trap: absurd value at physical {:08X}", physical_byte_address);
  }
  for (const auto& [start, length] : trap.ranges) {
    if (physical_byte_address >= start && physical_byte_address < start + length) {
      return;
    }
  }
  if (trap.ranges.size() >= 6) {
    return;
  }
  const uint32_t page = physical_byte_address & ~0xFFFu;
  const uint32_t start = page >= 0x8000u ? page - 0x8000u : 0u;
  trap.ranges.emplace_back(start, 0x14000u);  // 20 pages around it
  trap.kinds.push_back("vertices");
  trap.armed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("plume: vertex trap: watching physical {:08X}..{:08X}", start, start + 0x14000u);
}

void TrapRearm(memory::Memory* memory) {
  auto& trap = Trap();
  if (!memory || !trap.armed.load(std::memory_order_relaxed)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  {
    std::lock_guard lock(trap.mutex);
    ranges = trap.ranges;
  }
  for (const auto& [start, length] : ranges) {
    memory->EnablePhysicalMemoryAccessCallbacks(start, length, true, false);
  }
}

// From the write callback, on the writing thread, before the write goes through.
void TrapHit(uint32_t start, uint32_t length) {
#if defined(__linux__) && !defined(__ANDROID__)  // bionic: backtrace() only from API 33
  auto& trap = Trap();
  bool inside = false;
  const char* kind = "vertices";
  {
    std::lock_guard lock(trap.mutex);
    for (size_t r = 0; r < trap.ranges.size(); ++r) {
      const auto& [range_start, range_length] = trap.ranges[r];
      if (start < range_start + range_length && start + std::max(length, 1u) > range_start) {
        inside = true;
        kind = trap.kinds[r];
        break;
      }
    }
  }
  if (!inside) {
    return;
  }
  void* frames[24];
  const int count = backtrace(frames, 24);
  uint64_t signature = 0;
  for (int i = 3; i < count && i < 12; ++i) {
    signature = signature * 1099511628211ull ^ reinterpret_cast<uintptr_t>(frames[i]);
  }
  signature ^= reinterpret_cast<uintptr_t>(kind);
  const uint32_t event = trap.events.fetch_add(1, std::memory_order_relaxed);
  bool first = false;
  uint32_t seen = 0;
  {
    std::lock_guard lock(trap.mutex);
    uint32_t& writes = trap.writers[signature];
    first = writes++ == 0 && trap.writers.size() <= 64;
    seen = writes;
  }
  if (first) {
    std::string stack;
    for (int i = 2; i < count; ++i) {
      Dl_info info{};
      if (dladdr(frames[i], &info) && info.dli_fname) {
        const char* slash = std::strrchr(info.dli_fname, '/');
        stack += fmt::format(" {}+{:X}", slash ? slash + 1 : info.dli_fname,
                             reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase));
        if (info.dli_sname) {
          stack += fmt::format("({})", info.dli_sname);
        }
      }
    }
    REXLOG_INFO("plume: vertex trap: a new writer #{} of {} at physical {:08X}+{}, event {}:{}", trap.writers.size(),
                kind, start, length, event, stack);
  } else if ((event % 2000) == 1999) {
    REXLOG_INFO("plume: vertex trap: {} events, {} distinct writers", event + 1, trap.writers.size());
  }
  (void)seen;
#else
  (void)start;
  (void)length;
#endif
}

// The Direct3D device the title draws with (its shader constants live at device + 0x780, see
// plume_graphics_system.cpp); told to us when the graphics system finds it.
std::atomic<uint32_t> g_trap_device{0};

// The vertex shader constants shadow, 4 KB from device + 0x780: watched, so the writers of a constant register that turns
// out absurd (see CheckShaderConstants) show up. Only the first write to a page after each re-arm is seen.
void TrapArmConstants() {
  const uint32_t device = g_trap_device.load(std::memory_order_relaxed);
  if (device == 0) {
    return;
  }
  // Guest virtual to physical, as the vertex cache does it.
  const uint32_t constants = device + 0x780u;
  const uint32_t physical = (constants & 0x1FFFFFFFu) + (((constants >> 20) + 0x200u) & 0x1000u);
  const uint32_t start = physical & ~0xFFFu;
  auto& trap = Trap();
  std::lock_guard lock(trap.mutex);
  for (const char* kind : trap.kinds) {
    if (std::strcmp(kind, "constants") == 0) {
      return;
    }
  }
  trap.ranges.emplace_back(start, 0x3000u);
  trap.kinds.push_back("constants");
  trap.armed.store(true, std::memory_order_relaxed);
  REXLOG_INFO("plume: vertex trap: watching the shader constants, physical {:08X}..{:08X} (device {:08X})", start,
              start + 0x3000u, device);
}

// XERENGE_CONSTANT_TRACE (the launcher's debug_constant_trace): an absurd value (NaN, infinity, beyond 1e8) in one of the
// vertex shader's placement-matrix registers - the ones that carry object and bone transforms - is logged once per shader
// and register with the 4x4 block it sits in and the draw, and starts the writer trap on the constants.
void CheckShaderConstants(const GuestDrawSnapshot& snap) {
  static const bool on = std::getenv("XERENGE_CONSTANT_TRACE") != nullptr;
  if (!on) {
    return;
  }
  const ShaderSourceInfo* info = FindShaderSourceInfo(snap.vs_hash);
  if (!info || info->matrix_registers.empty()) {
    return;
  }
  const float* constants = reinterpret_cast<const float*>(snap.vs_constants.data());
  for (uint16_t reg : info->matrix_registers) {
    if (reg >= 256) {
      continue;
    }
    bool absurd = false;
    for (int i = 0; i < 4; ++i) {
      const float x = constants[size_t(reg) * 4 + i];
      absurd = absurd || !std::isfinite(x) || std::fabs(x) > 1.0e8f;
    }
    if (!absurd) {
      continue;
    }
    static std::mutex seen_mutex;
    static std::set<uint64_t> seen;
    bool fresh = false;
    {
      std::lock_guard lock(seen_mutex);
      fresh = seen.size() < 96 && seen.insert(snap.vs_hash ^ (uint64_t(reg) << 52)).second;
    }
    if (fresh) {
      const uint32_t base = reg & ~3u;
      std::string block;
      for (uint32_t r = base; r < base + 4 && r < 256; ++r) {
        block += fmt::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", r, constants[r * 4], constants[r * 4 + 1],
                             constants[r * 4 + 2], constants[r * 4 + 3]);
      }
      REXLOG_WARN(
          "plume: absurd shader constant vs={:016X} c{} in the block at c{}:{} - placement registers {} (c{}..c{}), "
          "prim {} indices {} vb {:08X} stride {}",
          snap.vs_hash, reg, base, block, info->matrix_registers.size(), info->matrix_registers.front(),
          info->matrix_registers.back(), snap.prim_type, snap.num_indices, snap.d3d_vertex_buffer,
          snap.d3d_vertex_stride);
    }
    TrapArmConstants();
    return;
  }
}
}  // namespace

void NoteD3DDeviceForTrap(uint32_t device_guest) {
  g_trap_device.store(device_guest, std::memory_order_relaxed);
}

constexpr uint32_t kWatchedPages = 0x20000000u >> 12;  // 512 MB of physical memory

std::pair<uint32_t, uint32_t> PlumeDrawContext::OnGuestWrite(void* context, uint32_t start,
                                                             uint32_t length, bool exact_range) {
  (void)exact_range;
  auto* self = static_cast<PlumeDrawContext*>(context);
  if (Trap().armed.load(std::memory_order_relaxed)) {
    TrapHit(start, length);
  }
  if (self->page_writes_ && length != 0) {
    const uint32_t first = (start & 0x1FFFFFFFu) >> 12;
    const uint64_t last = (uint64_t(start & 0x1FFFFFFFu) + length - 1) >> 12;
    for (uint64_t page = first; page <= last && page < kWatchedPages; ++page) {
      self->page_writes_[page].fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Only what was written: were more unwatched with it, a write to one of
  // those pages before it is watched again would go unnoticed.
  return {start, length};
}

uint64_t PlumeDrawContext::PageWrites(uint32_t start, size_t size) const {
  uint64_t sum = 0;
  if (!page_writes_ || size == 0) {
    return sum;
  }
  const uint32_t first = (start & 0x1FFFFFFFu) >> 12;
  const uint64_t last = (uint64_t(start & 0x1FFFFFFFu) + size - 1) >> 12;
  for (uint64_t page = first; page <= last && page < kWatchedPages; ++page) {
    sum += page_writes_[page].load(std::memory_order_relaxed);
  }
  return sum;
}

void PlumeDrawContext::BindGuestTextures(plume::RenderCommandList* list,
                                         const std::vector<GuestDrawSnapshot>& draws,
                                         memory::Memory* memory) {
  if (!list || !memory || !texture_set_ || !device_) {
    return;
  }
  using rex::graphics::xenos::FetchConstantType;
  using rex::graphics::xenos::TextureFormat;
  textures_done_this_frame_.clear();
  EncodeStage("textures", nullptr);
  TrapRearm(memory);
  // The vertex cache starts over when nearly full; the previous frame has
  // finished on the GPU by now, so nothing still reads what is overwritten.
  // Also when the one-off draws' markers pile up in the table.
  if (mesh_seen_once_.size() > 65536) {
    mesh_seen_once_.clear();
  }
  if (mesh_cache_.size() > 262144) {
    ++cache_resets_;
    REXLOG_INFO("plume: vertex cache full ({} vertices, {} meshes), starting over (#{})",
                cache_used_, mesh_cache_.size(), cache_resets_);
    mesh_cache_.clear();
    cache_by_offset_.clear();
    cache_used_ = 0;
  }
  TrimDrawSets();
  ++frame_serial_;
  size_t last_video = draws.size();
  for (size_t i = 0; i < draws.size(); ++i) {
    if (draws[i].has_video_frame()) {
      last_video = i;
    }
  }
  // Whether a texture changed is told by hashing its guest bytes, every frame,
  // for every texture the frame uses: tens of megabytes in the race, most of
  // this stage. Hashed here on the worker threads first, all at once; the
  // loop below takes the result.
  std::unordered_map<uint64_t, uint64_t> frame_hashes;
  {
    static const bool watching = std::getenv("XERENGE_NO_WRITE_WATCH") == nullptr;
    if (watching && !page_writes_ && memory) {
      page_writes_.reset(new std::atomic<uint32_t>[kWatchedPages]);
      for (uint32_t i = 0; i < kWatchedPages; ++i) {
        page_writes_[i].store(0, std::memory_order_relaxed);
      }
      watched_memory_ = memory;
      write_watch_handle_ = memory->RegisterPhysicalMemoryInvalidationCallback(&OnGuestWrite, this);
    }
    struct HashJob {
      uint64_t key;
      const uint8_t* src;
      size_t size;
      uint64_t hash = 0;
      uint64_t writes = 0;
      const uint8_t* mip_src = nullptr;
      size_t mip_size = 0;
    };
    std::vector<HashJob> jobs;
    std::unordered_set<uint64_t> queued;
    for (const GuestDrawSnapshot& snap : draws) {
      if (!snap.valid || snap.has_video_frame()) {
        continue;
      }
      const uint32_t used_slots = UsedTextureSlots(snap);
      for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
        if ((used_slots & (1u << slot)) == 0) {
          continue;
        }
        const auto fetch = TextureFetchAt(snap, slot);
        if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
          continue;
        }
        const uint64_t key = TextureKey(fetch);
        if (!queued.insert(key).second) {
          continue;
        }
        EncodeStage("textures: prepare", nullptr);
        rex::graphics::TextureInfo info{};
        if (!rex::graphics::TextureInfo::Prepare(fetch, &info)) {
          continue;
        }
        const uint32_t width = info.width + 1;
        const uint32_t height = info.height + 1;
        if (width > 2048 || height > 2048) {
          continue;
        }
        // The video's planes are written every video frame and never uploaded
        // (the loop below skips them): neither hashed nor watched.
        const auto base_format = rex::graphics::GetBaseFormat(fetch.format);
        if ((IsLumaFormat(fetch.format) && base_format != TextureFormat::k_8_A && width >= 640 &&
             height >= 360) ||
            IsPackedUvFormat(fetch.format)) {
          continue;
        }
        const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(info.memory.base_address);
        const size_t size = GuestTextureSourceSize(info);
        if (!src || !size) {
          continue;
        }
        // The mips are part of the texture: a change to them alone must
        // re-upload it just the same.
        EncodeStage("textures: mips", nullptr);
        GuestMipSource mips;
        const uint8_t* mip_src = nullptr;
        if (LocateGuestMips(fetch, &mips)) {
          mip_src = memory->TranslatePhysical<const uint8_t*>(mips.address);
        }
        const size_t mip_size = mip_src ? mips.size : 0;
        if (page_writes_) {
          // Still watched since it was last hashed: the watch comes off a page
          // only when the page is written, which counts. Set again every frame
          // regardless, it took the heap's lock and an mprotect per texture.
          {
            uint64_t writes = PageWrites(info.memory.base_address, size);
            if (mip_size) {
              writes += PageWrites(mips.address, mip_size);
            }
            if (auto known = texture_watch_.find(key);
                known != texture_watch_.end() && known->second.writes == writes) {
              frame_hashes.emplace(key, known->second.hash);
              continue;
            }
          }
          // Watched first, then the writes counted, then (if they changed)
          // hashed: a write at any point after the watch is set shows in the
          // count by the next frame at the latest.
          EncodeStage("textures: watching", nullptr);
          memory->EnablePhysicalMemoryAccessCallbacks(info.memory.base_address, uint32_t(size),
                                                      true, false);
          uint64_t writes = PageWrites(info.memory.base_address, size);
          if (mip_size) {
            memory->EnablePhysicalMemoryAccessCallbacks(mips.address, uint32_t(mip_size), true,
                                                        false);
            writes += PageWrites(mips.address, mip_size);
          }
          EncodeStage("textures", nullptr);
          if (auto known = texture_watch_.find(key);
              known != texture_watch_.end() && known->second.writes == writes) {
            frame_hashes.emplace(key, known->second.hash);
            continue;
          }
          jobs.push_back({key, src, size, 0, writes, mip_src, mip_size});
        } else {
          jobs.push_back({key, src, size, 0, 0, mip_src, mip_size});
        }
      }
    }
    EncodeStage("textures: hashing");
    WorkerPool::Get().ParallelFor(jobs.size(), [&](size_t i) {
      jobs[i].hash = TextureContentHash(jobs[i].src, jobs[i].size, jobs[i].mip_src,
                                        jobs[i].mip_size);
    });
    EncodeStage("textures: binding");
    frame_hashes.reserve(frame_hashes.size() + jobs.size());
    for (const HashJob& job : jobs) {
      frame_hashes.emplace(job.key, job.hash);
      if (page_writes_) {
        texture_watch_[job.key] = {job.hash, job.writes};
      }
    }
    {
      static uint64_t frames = 0;
      static uint64_t hashed = 0, reused = 0;
      hashed += jobs.size();
      reused += frame_hashes.size() - jobs.size();
      if (++frames % 600 == 0) {
        REXLOG_INFO("plume: texture checks - {} hashed, {} unchanged by the write watch",
                    hashed, reused);
      }
    }
  }

  for (size_t snap_index = 0; snap_index < draws.size(); ++snap_index) {
    const GuestDrawSnapshot& snap = draws[snap_index];
    if (!snap.valid || Skipped(snap_index)) {
      continue;
    }
    // Every copy of the decoded frame shares one texture, uploaded once from
    // the last; the earlier ones must not bind the raw planes instead.
    // From stage 2 the planes go up too, each packed into RGBA8 under a key
    // of its own; from stage 3 they are all there is.
    const auto upload_planes = [&] {
      const auto upload = [&](uint64_t key, uint32_t width, uint32_t height, uint32_t row_texels,
                              uint32_t bytes_per_sample, const std::vector<uint8_t>& bytes) {
        // A clip of another size in the same decoder buffer: the texture made
        // for the last one no longer fits, and refusing the upload left it
        // drawn - the old frame's planes sampled at the new frame's size.
        if (auto it = guest_textures_.find(PackedPlaneKey(key));
            it != guest_textures_.end() && it->second &&
            (it->second->width != PackedPlaneWidth(width, bytes_per_sample) ||
             it->second->height != height)) {
          texture_set_->setTexture(it->second->bindless, null_texture_.get(),
                                   plume::RenderTextureLayout::SHADER_READ, null_texture_view_.get());
          guest_textures_.erase(it);
        }
        const bool ok = UploadHostTexture(list, PackedPlaneKey(key),
                                          PackedPlaneWidth(width, bytes_per_sample), height,
                                          plume::RenderFormat::R8G8B8A8_UNORM, bytes,
                                          row_texels * bytes_per_sample / 4);
        if (!ok) {
          static uint32_t shown = 0;
          if (shown < 16) {
            ++shown;
            REXLOG_WARN("plume: video plane {:016X} not uploaded: {}x{} row {} texels, {} bytes",
                        key, PackedPlaneWidth(width, bytes_per_sample), height,
                        row_texels * bytes_per_sample / 4, bytes.size());
          }
        }
        return ok;
      };
      return upload(snap.video_key, snap.video_width, snap.video_height, snap.video_y_row_texels, 1,
                    snap.video_y) &&
             upload(snap.video_u_key, snap.video_u_width, snap.video_u_height,
                    snap.video_u_row_texels, snap.video_packed_uv ? 2 : 1, snap.video_u) &&
             (snap.video_packed_uv ||
              upload(snap.video_v_key, snap.video_v_width, snap.video_v_height,
                     snap.video_v_row_texels, 1, snap.video_v));
    };
    const auto upload_frame = [&] {
      bool ok = true;
      if (VideoGpuStage() < 3) {
        ok = UploadHostTexture(list, snap.video_key, snap.video_width, snap.video_height,
                               plume::RenderFormat::R8G8B8A8_UNORM, snap.video_rgba,
                               snap.video_row_texels);
      }
      if (VideoGpuStage() >= 2 && !snap.video_y.empty()) {
        const bool planes = upload_planes();
        ok = VideoGpuStage() >= 3 ? planes : ok;
      }
      return ok;
    };
    const bool captured =
        snap.has_video_frame() && (snap_index != last_video || upload_frame());
    // Every two seconds while a video is on screen: whether its frames change
    // (distinct luma hashes), what the planes look like and where they are
    // bound - enough to tell a decoder handing over the same frame from an
    // upload that does not land or a draw bound to the wrong texture.
    if (snap_index == last_video && !snap.video_y.empty()) {
      static uint64_t uploads = 0;
      static uint64_t last_hash = 0;
      static uint64_t distinct = 0;
      static uint64_t last_ms = 0;
      ++uploads;
      const uint64_t hash = XXH3_64bits(snap.video_y.data(), snap.video_y.size());
      if (hash != last_hash) {
        ++distinct;
        last_hash = hash;
      }
      const uint64_t now = CoarseMs();
      if (now - last_ms >= 2000) {
        last_ms = now;
        REXLOG_INFO("plume: video: {} uploads, {} distinct frames; Y {}x{} row {} key {:016X} "
                    "slot {}, U {}x{} row {} key {:016X} slot {}{}, V {}x{} row {} key {:016X} slot {}, captured {}, video pipeline {}",
                    uploads, distinct, snap.video_width, snap.video_height,
                    snap.video_y_row_texels, snap.video_key, VideoPlaneSlot(snap.video_key),
                    snap.video_u_width, snap.video_u_height, snap.video_u_row_texels,
                    snap.video_u_key, VideoPlaneSlot(snap.video_u_key),
                    snap.video_packed_uv ? " (Cb,Cr packed)" : "", snap.video_v_width, snap.video_v_height,
                    snap.video_v_row_texels, snap.video_v_key, VideoPlaneSlot(snap.video_v_key), captured,
                    UsesVideoPipeline(snap));
        uploads = 0;
        distinct = 0;
      }
    }
    const uint32_t used_slots = UsedTextureSlots(snap);
    for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
      if (captured && slot < 3) {
        continue;
      }
      if ((used_slots & (1u << slot)) == 0) {
        continue;
      }
      const auto fetch = TextureFetchAt(snap, slot);
      if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
        continue;
      }
      EnsureSampler(snap, slot);
      // Before Prepare, which runs for every texture slot of every draw while
      // a frame's draws overwhelmingly rebind the same few textures.
      const uint64_t key = TextureKey(fetch);
      if (std::find(textures_done_this_frame_.begin(), textures_done_this_frame_.end(), key) !=
          textures_done_this_frame_.end()) {
        continue;
      }
      textures_done_this_frame_.push_back(key);
      if (auto used = guest_textures_.find(key); used != guest_textures_.end() && used->second) {
        used->second->last_used = frame_serial_;
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
      const auto base_fmt_early = rex::graphics::GetBaseFormat(fetch.format);
      const bool video_luma_plane =
          IsLumaFormat(fetch.format) && base_fmt_early != TextureFormat::k_8_A && width >= 640 &&
          height >= 360;
      if (video_luma_plane || IsPackedUvFormat(fetch.format)) {
        continue;
      }
      plume::RenderFormat host_format = plume::RenderFormat::R8G8B8A8_UNORM;
      bool expand_r8 = false;
      if (!MapHostFormat(fetch.format, &host_format, &expand_r8)) {
        static uint32_t skip_logs = 0;
        if (skip_logs < 16) {
          ++skip_logs;
          REXLOG_INFO("plume: skip tex fmt={} addr={:08X}", uint32_t(fetch.format),
                      fetch.base_address);
        }
        continue;
      }
      const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(info.memory.base_address);
      if (!src) {
        continue;
      }
      // Every layer, for the shaders that sample it as a volume. The 2D view
      // below keeps layer 0 for those that do not.
      if (info.depth > 0 && volume_set_) {
        const TextureFormat layered_fmt = rex::graphics::GetBaseFormat(fetch.format);
        UploadLayeredTexture(list, key, info, src, host_format, expand_r8,
                             layered_fmt == TextureFormat::k_8_8_8_8 ||
                                 layered_fmt == TextureFormat::k_8_8_8_8_A);
      }

      // Untiling and uploading is expensive, and between frames most guest
      // textures do not change at all - repeating it for identical bytes is
      // what made the interface crawl.
      const size_t source_size = GuestTextureSourceSize(info);
      // Only plain 2D textures get their mips; volumes and the video's
      // formats keep the base alone.
      const TextureFormat mip_fmt = rex::graphics::GetBaseFormat(fetch.format);
      GuestMipSource mips;
      const uint8_t* mip_src = nullptr;
      if (info.depth == 0 && mip_fmt != TextureFormat::k_Cr_Y1_Cb_Y0_REP &&
          mip_fmt != TextureFormat::k_Y1_Cr_Y0_Cb_REP && LocateGuestMips(fetch, &mips)) {
        mip_src = memory->TranslatePhysical<const uint8_t*>(mips.address);
      }
      uint64_t content = 0;
      if (auto hashed = frame_hashes.find(key); hashed != frame_hashes.end()) {
        content = hashed->second;
      } else if (source_size) {
        content = TextureContentHash(src, source_size, mip_src, mip_src ? mips.size : 0);
      }
      if (content != 0) {
        auto it = guest_textures_.find(key);
        if (it != guest_textures_.end() && it->second && it->second->uploaded &&
            it->second->content_hash == content) {
          continue;
        }
      }

      std::vector<uint8_t> pixels;
      uint32_t row_bytes = 0;
      uint32_t row_texels = 0;
      const TextureFormat base_fmt = rex::graphics::GetBaseFormat(fetch.format);
      const bool alpha_mask = base_fmt == TextureFormat::k_8_A;
      if (!UntileGuestTexture(info, src, &pixels, &row_bytes, &row_texels, expand_r8, alpha_mask) ||
          pixels.empty() || row_bytes == 0) {
        continue;
      }
      const bool swizzle_bgra =
          base_fmt == TextureFormat::k_8_8_8_8 || base_fmt == TextureFormat::k_8_8_8_8_A;
      bool forced_opaque = false;
      if (swizzle_bgra) {
        SwizzleBgraToRgba(&pixels, height, row_bytes);
        forced_opaque = ForceOpaqueIfXrgb(&pixels, width, height, row_bytes);
      } else if (base_fmt == TextureFormat::k_Cr_Y1_Cb_Y0_REP ||
                 base_fmt == TextureFormat::k_Y1_Cr_Y0_Cb_REP) {
        if (!Convert422ToRgba(base_fmt, width, height, &pixels, &row_bytes, &row_texels)) {
          continue;
        }
      }

      bool dumped_now = false;
      static uint32_t dump_count = 0;
      static std::vector<uint32_t> dumped_addrs;
      static const bool dump_enabled = std::getenv("XERENGE_DUMP_TEXTURES") != nullptr;
      // XERENGE_DUMP_TEXTURES=<hex>,<hex>...: only textures at these physical
      // addresses (as the queue dump prints them); any other value, the first 256.
      static const std::vector<uint32_t> dump_only = [] {
        std::vector<uint32_t> addresses;
        if (const char* list = std::getenv("XERENGE_DUMP_TEXTURES")) {
          for (const char* p = list; *p;) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(p, &end, 16);
            if (end == p) {
              break;
            }
            if (value > 1) {
              addresses.push_back(uint32_t(value));
            }
            p = *end == ',' ? end + 1 : end;
          }
        }
        return addresses;
      }();
      const bool wanted =
          dump_only.empty() ||
          std::find(dump_only.begin(), dump_only.end(), info.memory.base_address) !=
              dump_only.end();
      if (dump_enabled && wanted && dump_count < 256 &&
          std::find(dumped_addrs.begin(), dumped_addrs.end(), info.memory.base_address) ==
              dumped_addrs.end()) {
        dumped_addrs.push_back(info.memory.base_address);
        dumped_now = true;
        char path[256];
        std::snprintf(path, sizeof(path), "logs/tex_%03u_%ux%u_fmt%u_addr%08X_%s.png", dump_count,
                     width, height, uint32_t(fetch.format), info.memory.base_address,
                     expand_r8 ? "r8" : "rgba");
        std::vector<uint8_t> decoded;
        if (DecodeCompressedForDump(base_fmt, pixels, row_bytes, width, height, &decoded)) {
          DumpTextureToPng(path, decoded.data(), width, height, width * 4);
        } else {
          DumpTextureToPng(path, pixels.data(), width, height, row_bytes);
        }
        ++dump_count;
      }
      // Without its mips a texture seen from afar is sampled at full size, and
      // the road and the buildings in the distance glitter and crawl.
      std::vector<HostMipLevel> mip_levels;
      if (mip_src) {
        DecodeGuestMips(fetch, mips, mip_src, info.endianness, expand_r8, alpha_mask,
                        swizzle_bgra, forced_opaque, &mip_levels);
      }
      // Its mips too, when the texture was dumped: a sprite drawn smaller than
      // its texture is sampled from them, and a bad mip shows only there.
      if (dumped_now) {
        const uint32_t block_bytes = base_fmt == TextureFormat::k_DXT1 ? 8u : 16u;
        for (size_t level = 0; level < mip_levels.size(); ++level) {
          const HostMipLevel& mip = mip_levels[level];
          if (mip.width == 0 || mip.height == 0) {
            continue;
          }
          char mip_path[256];
          std::snprintf(mip_path, sizeof(mip_path), "logs/tex_addr%08X_mip%zu_%ux%u.png",
                        info.memory.base_address, level + 1, mip.width, mip.height);
          std::vector<uint8_t> decoded;
          if (DecodeCompressedForDump(base_fmt, mip.pixels, mip.row_texels / 4 * block_bytes,
                                      mip.width, mip.height, &decoded)) {
            DumpTextureToPng(mip_path, decoded.data(), mip.width, mip.height, mip.width * 4);
          } else if (swizzle_bgra &&
                     mip.pixels.size() >= size_t(mip.row_texels) * mip.height * 4) {
            DumpTextureToPng(mip_path, mip.pixels.data(), mip.width, mip.height,
                             mip.row_texels * 4);
          }
        }
      }
      // A GPU without BC (DXT) textures - Mali, and most phones' drivers: the
      // blocks are decoded here into RGBA8, every mip with them.
      if (!bc_supported_ && (host_format == plume::RenderFormat::BC1_UNORM ||
                             host_format == plume::RenderFormat::BC2_UNORM ||
                             host_format == plume::RenderFormat::BC3_UNORM)) {
        const uint32_t block_bytes = base_fmt == TextureFormat::k_DXT1 ? 8u : 16u;
        std::vector<uint8_t> decoded;
        int etc2_alpha = -1;
        uint32_t etc2_row = 0;
        if (etc2_supported_ && TranscodeBcToEtc2(base_fmt, pixels, row_bytes, width, height,
                                                 &etc2_alpha, &decoded, &etc2_row)) {
          // Every level in the base level's format: one image, one format.
          pixels = std::move(decoded);
          row_texels = etc2_row;
          row_bytes = etc2_row / 4 * (etc2_alpha ? 16u : 8u);
          for (HostMipLevel& mip : mip_levels) {
            std::vector<uint8_t> level;
            uint32_t level_row = 0;
            if (mip.width != 0 && mip.height != 0 &&
                TranscodeBcToEtc2(base_fmt, mip.pixels, mip.row_texels / 4 * block_bytes,
                                  mip.width, mip.height, &etc2_alpha, &level, &level_row)) {
              mip.pixels = std::move(level);
              mip.row_texels = level_row;
            }
          }
          host_format = etc2_alpha ? plume::RenderFormat::ETC2_RGBA8_UNORM
                                   : plume::RenderFormat::ETC2_RGB8_UNORM;
        } else if (DecodeCompressedForDump(base_fmt, pixels, row_bytes, width, height, &decoded)) {
          pixels = std::move(decoded);
          row_bytes = width * 4;
          row_texels = width;
          for (HostMipLevel& mip : mip_levels) {
            std::vector<uint8_t> level;
            if (mip.width != 0 && mip.height != 0 &&
                DecodeCompressedForDump(base_fmt, mip.pixels, mip.row_texels / 4 * block_bytes,
                                        mip.width, mip.height, &level)) {
              mip.pixels = std::move(level);
              mip.row_texels = mip.width;
            }
          }
          host_format = plume::RenderFormat::R8G8B8A8_UNORM;
        }
      }
      if (!UploadHostTexture(list, key, width, height, host_format, pixels, row_texels,
                             &mip_levels)) {
        continue;
      }
      // Remember what was uploaded so the next frame can tell this texture is
      // unchanged without redoing any of the work above.
      if (auto it = guest_textures_.find(key); it != guest_textures_.end() && it->second) {
        it->second->content_hash = content;
      }
      // XERENGE_DUMP_STRIPS: a packed-mip strip as untiled with and without
      // the packed tail's offset, to see which one holds the picture.
      static const bool dump_strips = std::getenv("XERENGE_DUMP_STRIPS") != nullptr;
      if (dump_strips && height <= 16 && width >= 64 && info.has_packed_mips && info.is_tiled) {
        static std::set<uint32_t> dumped;
        if (dumped.size() < 16 && dumped.insert(info.memory.base_address).second) {
          rex::graphics::TextureInfo flat = info;
          flat.has_packed_mips = false;
          flat.height = 31;  // the whole 32-row tile
          std::vector<uint8_t> raw_tile;
          uint32_t rb2 = 0, rt2 = 0;
          if (UntileGuestTexture(flat, src, &raw_tile, &rb2, &rt2, false) && !raw_tile.empty()) {
            if (base_fmt == TextureFormat::k_8_8_8_8) {
              SwizzleBgraToRgba(&raw_tile, 32, rb2);
            }
            char path[160];
            std::snprintf(path, sizeof(path), "logs/strip_%08X_tile.png", info.memory.base_address);
            DumpTextureToPng(path, raw_tile.data(), width, 32, rb2);
            std::snprintf(path, sizeof(path), "logs/strip_%08X_used.png", info.memory.base_address);
            DumpTextureToPng(path, pixels.data(), width, height, row_bytes);
            uint32_t px = 0, py = 0, pz = 0;
            rex::graphics::texture_util::GetPackedMipOffset(width, height, 1, info.format, 0, px, py, pz);
            REXLOG_INFO("plume: strip {:08X} {}x{} fmt={} packed offset ({}, {}) blocks",
                        info.memory.base_address, width, height, uint32_t(fetch.format), px, py);
          }
        }
      }
      if (dump_strips && height == 8 && width == 256 && !info.is_tiled) {
        static std::set<uint32_t> raw_dumped;
        if (raw_dumped.size() < 2 && raw_dumped.insert(info.memory.base_address).second) {
          std::string rows;
          for (uint32_t y = 0; y < 8; ++y) {
            rows += fmt::format(" row{}:", y);
            for (uint32_t x : {0u, 64u, 127u, 128u, 192u, 255u}) {
              const uint8_t* t = src + y * 1024 + x * 4;
              rows += fmt::format(" {:02X}{:02X}{:02X}{:02X}", t[0], t[1], t[2], t[3]);
            }
          }
          REXLOG_INFO("plume: palette {:08X} endian {} raw{}", info.memory.base_address,
                      uint32_t(info.endianness), rows);
        }
      }
      // Palette-like strips: a colour table the car shaders index by row.
      if (height <= 32 && width >= 64) {
        static uint32_t strip_logs = 0;
        if (strip_logs < 24) {
          ++strip_logs;
          REXLOG_INFO("plume: strip tex {}x{} fmt={} tiled={} pitch={} packed_mips={} addr={:08X} "
                      "dim={} depth={}",
                      width, height, uint32_t(fetch.format), info.is_tiled ? 1 : 0, info.pitch,
                      info.has_packed_mips ? 1 : 0, info.memory.base_address,
                      uint32_t(fetch.dimension), info.depth + 1);
        }
      }
      static uint32_t tex_logs = 0;
      if (tex_logs < 16 || (expand_r8 && tex_logs < 24)) {
        ++tex_logs;
        REXLOG_INFO("plume: guest tex slot={} {}x{} fmt={} tiled={} addr={:08X}", slot, width,
                    height, uint32_t(fetch.format), info.is_tiled ? 1 : 0, info.memory.base_address);
      }
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

// A non-indexed Direct3D draw that samples a target copied earlier: the
// post-processing passes and the composite of the 3D frame.
bool PlumeDrawContext::ReadsResolvedCopy(const GuestDrawSnapshot& snap,
                                         bool this_frame) const {
  if (snap.d3d_vertex_buffer == 0 || snap.d3d_index_buffer != 0) {
    return false;
  }
  // Slot 0 only: that is where these passes take the picture they work on.
  // The other slots keep whatever the scene left in them - its cube map copy
  // among them - so scanning them all counted every interface draw as a pass
  // over a copy, and its positions were converted twice and collapsed.
  using rex::graphics::xenos::FetchConstantType;
  const auto fetch = TextureFetchAt(snap, 0);
  if (fetch.type == FetchConstantType::kTexture && fetch.base_address != 0) {
    auto it = resolved_targets_.find(fetch.base_address << 12);
    if (it != resolved_targets_.end() && !it->second.cube &&
        (!this_frame || frame_resolved_dests_.count(fetch.base_address << 12) != 0)) {
      return true;
    }
  }
  return false;
}

uint32_t PlumeDrawContext::BindlessForSlot(const GuestDrawSnapshot& snap, uint32_t slot) const {
  using rex::graphics::xenos::FetchConstantType;
  const auto fetch = TextureFetchAt(snap, slot);
  if (fetch.type != FetchConstantType::kTexture || fetch.base_address == 0) {
    return 0;
  }
  // A real texture always wins. A resolved render target is only a stand-in
  // for an address this renderer has nothing else for: guest memory at a
  // resolve destination holds nothing it ever wrote, so untiling it gives
  // garbage. But an address can be both - the video decoder writes its frames
  // into memory the title also resolves into - and there the decoded frame is
  // the right answer, not a snapshot of the window.
  // For a draw built from the Direct3D calls the resolved picture comes first:
  // its guest memory was never written, the GPU would have written it, so a
  // texture untiled from there is stale. (The ring's draws keep the old order -
  // that is where the video frames arrive.)
  if (snap.d3d_vertex_buffer != 0) {
    if (auto it = resolved_targets_.find(fetch.base_address << 12);
        it != resolved_targets_.end()) {
      return it->second.bindless;
    }
  }
  const uint64_t key = TextureKey(fetch);
  if (auto it = guest_textures_.find(key); it != guest_textures_.end() && it->second) {
    return it->second->bindless;
  }
  // A texture fetch holds a page index while RB_COPY_DEST_BASE is a byte
  // address, hence the shift.
  if (auto it = resolved_targets_.find(fetch.base_address << 12);
      it != resolved_targets_.end()) {
    return it->second.bindless;
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
  const uint32_t used_slots = UsedTextureSlots(snap);
  for (uint32_t slot = 0; slot < 16; ++slot) {
    if ((used_slots & (1u << slot)) == 0) {
      continue;
    }
    words[slot] = BindlessForSlot(snap, slot);
    words[48 + slot] = 0;
    if (auto it = sampler_index_by_key_.find(SamplerKey(snap, slot));
        it != sampler_index_by_key_.end()) {
      words[48 + slot] = it->second;
    }
    // Cube maps have their own index, into the cube set (words 32 + slot).
    const auto fetch = TextureFetchAt(snap, slot);
    // So do layered textures, into the volume set (words 16 + slot); 0 there
    // is the identity colour table.
    if (fetch.base_address != 0) {
      if (auto it = layered_textures_.find(TextureKey(fetch));
          it != layered_textures_.end() && it->second.texture) {
        words[16 + slot] = it->second.index;
      }
    }
    if (fetch.base_address != 0 &&
        fetch.dimension == rex::graphics::xenos::DataDimension::kCube) {
      if (auto it = resolved_targets_.find(fetch.base_address << 12);
          it != resolved_targets_.end() && it->second.cube) {
        words[32 + slot] = it->second.bindless;
      }
    }
  }
  // A vertex shader's samplers. XenosRecomp numbers them from slot 0, as it
  // does the pixel shader's, but the title binds them to the vertex fetch
  // constants from 16 on. The sky's colour comes from a gradient sampled that
  // way; read through fetch constant 0 it took a sparkle texture instead, and
  // the sky went pink, white or blue with the view.
  const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
  const bool vertex_fetch = vs_info ? vs_info->is_vertex_fetch : false;
  if (vertex_fetch) {
    const uint32_t vs_known = ShaderSamplerSlots(snap.vs_hash);
    const uint32_t vs_slots = vs_known == ~0u ? 0u : vs_known;
    const uint32_t ps_slots = ShaderSamplerSlots(snap.ps_hash);
    for (uint32_t slot = 0; slot < 4; ++slot) {
      if ((vs_slots & (1u << slot)) == 0) {
        continue;
      }
      if ((ps_slots & (1u << slot)) != 0) {
        static std::atomic<uint32_t> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
          REXLOG_WARN("plume: vs={:016X} and ps={:016X} both sample slot {}; the pixel "
                      "shader's texture wins",
                      snap.vs_hash, snap.ps_hash, slot);
        }
        continue;
      }
      const uint32_t fetch_slot = 16 + slot;
      words[slot] = BindlessForSlot(snap, fetch_slot);
      words[48 + slot] = 0;
      if (auto it = sampler_index_by_key_.find(SamplerKey(snap, fetch_slot));
          it != sampler_index_by_key_.end()) {
        words[48 + slot] = it->second;
      }
    }
  }
  words[64] = snap.vs_bool;
  // Direct3D 9 samples at whole guest-pixel coordinates; the interface lays
  // its pieces out for that grid. Drawn f times larger (1920 from 1280: 1.5),
  // no shift matches that for every pixel, so pick the one whose samples stay
  // just inside each guest pixel - 0.1 of it past the left edge, i.e. a shift
  // of 0.5 - 0.1 f of our pixels (exactly half a pixel at f = 1). Half of our
  // pixel (0.5) put a sample at 0.667, inside the menu buttons' 0.653-0.678
  // overlap: a dark line down the middle of every button. Half of theirs
  // (0.75) left the first column's sample at -0.17, outside the full-screen
  // mask: a gap down the left edge.
  const auto shift = [](uint32_t host, uint32_t guest) {
    if (host == 0) {
      return 0.0f;
    }
    const float scale = guest ? float(host) / float(guest) : 1.0f;
    const float pixels = scale > 1.001f ? 0.5f - 0.1f * scale : 0.5f;
    return 2.0f * pixels / float(host);
  };
  const uint32_t grid_w = snap.d3d_target_width ? snap.d3d_target_width : width;
  const uint32_t grid_h = snap.d3d_target_height ? snap.d3d_target_height : height;
  float half_x = width ? shift(width, grid_w) : (1.0f / 1280.0f);
  float half_y = height ? shift(height, grid_h) : (1.0f / 720.0f);
  std::memcpy(dst + 280, &half_x, 4);
  std::memcpy(dst + 284, &half_y, 4);
  float alpha = snap.alpha_test ? snap.alpha_ref : 0.0f;
  std::memcpy(dst + 308, &alpha, 4);
}

namespace {
// Where the encoder is, for a watchdog that says so when it stops moving. The
// UI thread was found spinning at a full core with every guest thread asleep
// behind it; this names the step and the draw without stopping anything.
std::atomic<const char*> g_encode_stage{"idle"};
std::atomic<uint64_t> g_encode_stage_since_ms{0};
std::atomic<uint64_t> g_encode_vs{0};
std::atomic<uint64_t> g_encode_ps{0};
std::atomic<uint32_t> g_encode_indices{0};

uint64_t NowMs() { return CoarseMs(); }

// Time per encode stage, accumulated as the stage changes and reported every
// few seconds: where a frame's recording goes.
void AccountEncodeStage(const char* next) {
  if (!PlumeTiming()) {
    return;
  }
  static std::mutex stage_mutex;
  std::lock_guard stage_lock(stage_mutex);
  static const char* current = nullptr;
  static auto since = std::chrono::steady_clock::now();
  static std::map<const char*, uint64_t> totals;
  static auto last_report = since;
  static uint64_t frames = 0;
  static std::map<const char*, uint64_t> frame_totals;
  const auto now = std::chrono::steady_clock::now();
  if (current) {
    const uint64_t ns =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - since).count());
    totals[current] += ns;
    if (std::strcmp(current, "idle") != 0) {
      frame_totals[current] += ns;
    }
  }
  if (std::strcmp(next, "idle") == 0) {
    ++frames;
    // One slow frame on its own: an average over two seconds hides it.
    uint64_t frame_ns = 0;
    for (const auto& [stage, ns] : frame_totals) {
      frame_ns += ns;
    }
    if (frame_ns >= 100000000ull) {
      std::string out;
      for (const auto& [stage, ns] : frame_totals) {
        if (ns >= 1000000ull) {
          out += fmt::format(" {}={}ms", stage, ns / 1000000);
        }
      }
      REXLOG_WARN("plume: slow encode {} ms:{}", frame_ns / 1000000, out);
    }
    frame_totals.clear();
  }
  current = next;
  since = now;
  if (now - last_report >= std::chrono::seconds(2) && frames != 0) {
    std::string out;
    for (const auto& [stage, ns] : totals) {
      if (std::strcmp(stage, "idle") != 0) {
        out += fmt::format(" {}={}us", stage, ns / 1000 / frames);
      }
    }
    REXLOG_INFO("plume: encode stages per frame ({} frames):{}", frames, out);
    totals.clear();
    frames = 0;
    last_report = now;
  }
}

void EncodeStage(const char* stage, const GuestDrawSnapshot* snap) {
  // Per draw, so only in debug mode: the stage times and the stuck-encoder
  // watchdog (which ignores a stage with no time, as here) both read the clock.
  if (!xerenge::Diagnostics()) {
    g_encode_stage.store(stage, std::memory_order_release);
    return;
  }
  AccountEncodeStage(stage);
  if (snap) {
    g_encode_vs.store(snap->vs_hash, std::memory_order_relaxed);
    g_encode_ps.store(snap->ps_hash, std::memory_order_relaxed);
    g_encode_indices.store(snap->num_indices, std::memory_order_relaxed);
  }
  g_encode_stage_since_ms.store(NowMs(), std::memory_order_relaxed);
  g_encode_stage.store(stage, std::memory_order_release);
}

void StartEncodeWatchdog() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      const char* reported = nullptr;
      uint64_t reported_since = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const char* stage = g_encode_stage.load(std::memory_order_acquire);
        const uint64_t since = g_encode_stage_since_ms.load(std::memory_order_relaxed);
        if (std::strcmp(stage, "idle") == 0 || since == 0) {
          continue;
        }
        const uint64_t stuck = NowMs() - since;
        if (stuck >= 2000 && !(stage == reported && since == reported_since)) {
          reported = stage;
          reported_since = since;
          REXLOG_WARN("plume: encoder stuck {} ms in '{}' - draw vs={:016X} ps={:016X} "
                      "indices={}",
                      stuck, stage, g_encode_vs.load(), g_encode_ps.load(),
                      g_encode_indices.load());
        }
      }
    }).detach();
  });
}
}  // namespace

void PlumeDrawContext::ChooseReflectionFaces(const std::vector<GuestDrawSnapshot>& draws) {
  skip_draw_.clear();
  static const bool split = std::getenv("XERENGE_REFLECTION_SPLIT") != nullptr;
  if (!split) {
    return;
  }
  // A face is the draws into a target of its size since the copy before, and
  // its own copy into the cube; the faces not this frame's keep what they
  // were drawn with last time. Clears and the video stay: they may be the
  // frame's own, ahead of the first face.
  bool cube_seen = false;
  size_t face_start = 0;
  for (size_t i = 0; i < draws.size(); ++i) {
    const GuestDrawSnapshot& snap = draws[i];
    if (!snap.is_resolve) {
      continue;
    }
    if (snap.resolve_cube && snap.resolve_face < 6) {
      cube_seen = true;
      static uint32_t described = 0;
      if (xerenge::Diagnostics() && described < 12) {
        ++described;
        // What a face's draws say they draw into: matched against the copy's
        // size to tell them from the frame's own.
        std::map<std::tuple<uint32_t, uint32_t, bool, bool>, uint32_t> kinds;
        for (size_t j = face_start; j < i; ++j) {
          const GuestDrawSnapshot& face = draws[j];
          ++kinds[{face.d3d_target_width, face.d3d_target_height, face.is_clear,
                   face.d3d_vertex_buffer != 0}];
        }
        std::string text;
        for (const auto& [kind, count] : kinds) {
          text += fmt::format(" {}x{}{}{}:{}", std::get<0>(kind), std::get<1>(kind),
                              std::get<2>(kind) ? " clear" : "",
                              std::get<3>(kind) ? " d3d" : " ring", count);
        }
        REXLOG_INFO("plume: reflection face {} copy {}x{} (turn {}): draws by target{}",
                    snap.resolve_face, snap.resolve_width, snap.resolve_height, reflection_turn_,
                    text);
      }
      if (snap.resolve_face / 2 != reflection_turn_) {
        if (skip_draw_.empty()) {
          skip_draw_.assign(draws.size(), 0);
        }
        skip_draw_[i] = 1;
        for (size_t j = face_start; j < i; ++j) {
          const GuestDrawSnapshot& face = draws[j];
          if (!face.is_clear && !face.is_resolve && !face.has_video_frame() &&
              face.d3d_target_width == snap.resolve_width &&
              face.d3d_target_height == snap.resolve_height) {
            skip_draw_[j] = 1;
          }
        }
      }
    }
    face_start = i + 1;
  }
  if (cube_seen) {
    reflection_turn_ = (reflection_turn_ + 1) % 3;
  }
}

void PlumeDrawContext::EncodeDraws(plume::RenderCommandList* list,
                                   const std::vector<GuestDrawSnapshot>& draws,
                                   memory::Memory* memory, uint32_t width, uint32_t height,
                                   plume::RenderTexture* colour_target,
                                   const RenderPassBreak* pass) {
  uint32_t resolved_in_place = 0;
  if (!ready_ || !list || draws.empty()) {
    ClearFirstPass(list, pass, 0, 0);
    return;
  }

  UploadNullTexture(list);
  ChooseReflectionFaces(draws);
  BindGuestTextures(list, draws, memory);
  // Every draw's vertex cache check at once, on the worker threads: hashing
  // index buffers and the vertex ranges they read is most of what a cached
  // draw costs, and none of it depends on the draws before.
  EncodeStage("vertex cache checks");
  mesh_checks_.clear();
  if (cache_mapped_ && memory) {
    std::vector<const GuestDrawSnapshot*> candidates;
    candidates.reserve(draws.size());
    for (size_t index = 0; index < draws.size(); ++index) {
      const GuestDrawSnapshot& snap = draws[index];
      if (snap.valid && !snap.is_clear && !snap.is_resolve && snap.d3d_vertex_buffer != 0 &&
          !snap.has_video_frame() && !Skipped(index)) {
        candidates.push_back(&snap);
      }
    }
    std::vector<MeshCheck> results(candidates.size());
    WorkerPool::Get().ParallelFor(candidates.size(), [&](size_t i) {
      const GuestDrawSnapshot& snap = *candidates[i];
      const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
      if (vs_info && !vs_info->real_attrs.empty()) {
        results[i] = CheckMeshCache(snap, vs_info->real_attrs, snap.num_indices, memory);
      } else {
        results[i] = MeshCheck{};
      }
    });
    mesh_checks_.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
      mesh_checks_.emplace(candidates[i], results[i]);
      // Taken by this frame from here on: a mesh stored later in the frame
      // must not evict it before the draw that found it is encoded - that
      // draw would read someone else's vertices, and meshes burst apart.
      if (results[i].hit) {
        if (auto e = mesh_cache_.find(results[i].key); e != mesh_cache_.end()) {
          e->second.last_used = frame_serial_;
        }
      }
    }
  }
  // And the draws the cache does not have (the interface, particles, a mesh
  // seen for the first time), unpacked on the worker threads as well, each
  // into its own part of pre_arena_; FillVertices copies them from there.
  EncodeStage("unpacking ahead");
  pre_unpacked_.clear();
  static const bool unpack_ahead = std::getenv("XERENGE_NO_PARALLEL_UNPACK") == nullptr;
  if (unpack_ahead && memory) {
    struct Job {
      const GuestDrawSnapshot* snap = nullptr;
      const std::vector<VfetchAttr>* attrs = nullptr;
      int32_t pos_fetch_const = -1;
      uint32_t pos_float = 0;
      PreUnpacked result;
    };
    std::vector<Job> jobs;
    jobs.reserve(draws.size());
    size_t total = 0;
    for (size_t index = 0; index < draws.size(); ++index) {
      const GuestDrawSnapshot& snap = draws[index];
      // Nor a draw left out this frame (a reflection face drawn another
      // time): unpacked for nothing, the four faces skipped cost the phone
      // some 35 ms a frame of the worker threads' time.
      if (!snap.valid || snap.is_clear || snap.is_resolve || snap.d3d_vertex_buffer == 0 ||
          snap.d3d_vertex_stride == 0 || snap.has_video_frame() || snap.num_indices == 0 ||
          snap.num_indices > kVertsPerFrame || Skipped(index)) {
        continue;
      }
      if (auto check = mesh_checks_.find(&snap); check != mesh_checks_.end() && check->second.hit) {
        continue;
      }
      const auto* vs_info = FindResolvedVfetch(snap.vs_hash);
      if (!vs_info || vs_info->real_attrs.empty()) {
        continue;
      }
      Job job;
      job.snap = &snap;
      job.attrs = &vs_info->real_attrs;
      job.pos_fetch_const = vs_info->real_pos_fetch_const;
      job.pos_float = vs_info->real_pos_float;
      job.result.offset = total;
      job.result.count = snap.num_indices;
      total += size_t(snap.num_indices) * kFloatsPerVert;
      jobs.push_back(std::move(job));
    }
    // Bounded: an enormous frame is left to the ordinary path.
    if (!jobs.empty() && total <= (size_t(256) << 20) / sizeof(float)) {
      if (pre_arena_.size() < total) {
        pre_arena_.resize(total);
      }
      WorkerPool::Get().ParallelFor(jobs.size(), [&](size_t i) {
        Job& job = jobs[i];
        float* out = pre_arena_.data() + job.result.offset;
        std::memset(out, 0, size_t(job.result.count) * kVertexStrideBytes);
        job.result.read_lo.assign(job.attrs->size(), ~0u);
        job.result.read_hi.assign(job.attrs->size(), 0u);
        UnpackVertices(*job.snap, *job.attrs, job.pos_fetch_const, job.pos_float, job.result.count, memory, out,
                       job.result.read_lo, job.result.read_hi, job.result.fetched,
                       job.result.fetch_addr, job.result.fetch_type);
      });
      pre_unpacked_.reserve(jobs.size());
      for (Job& job : jobs) {
        pre_unpacked_.emplace(job.snap, std::move(job.result));
      }
    }
  }

  list->setGraphicsPipelineLayout(pipeline_layout_.get());

  uint32_t encoded = 0;
  // Frames alternate between two halves of the vertex and constant buffers:
  // the GPU may still be drawing the last frame from its half while this one
  // is written (XERENGE_ASYNC_PRESENT keeps a frame in flight).
  const uint32_t half = uint32_t(frame_serial_ & 1);
  const uint32_t vb_base = half * kVertsPerFrame;
  const uint32_t slot_base = half * kSlotsPerFrame;
  uint32_t vb_used = vb_base;
  vb_limit_ = vb_base + kVertsPerFrame;
  uint32_t guest_draws = 0;
  using rex::graphics::xenos::PrimitiveType;
  size_t last_video = draws.size();
  for (size_t i = 0; i < draws.size(); ++i) {
    if (draws[i].has_video_frame()) {
      last_video = i;
    }
  }
  struct BoundUiTex {
    uint64_t key = 0;
    uint32_t base = 0;
    uint32_t format = 0;
    uint32_t w = 0;
    uint32_t h = 0;
  };
  auto bound_ui_tex = [&](const GuestDrawSnapshot& s, BoundUiTex* out) -> bool {
    if (!out) {
      return false;
    }
    *out = BoundUiTex{};
    for (uint32_t slot = 0; slot < rex::graphics::xenos::kTextureFetchConstantCount; ++slot) {
      if (BindlessForSlot(s, slot) == 0) {
        continue;
      }
      const auto fetch = TextureFetchAt(s, slot);
      out->format = uint32_t(rex::graphics::GetBaseFormat(fetch.format));
      out->base = fetch.base_address;
      out->key = TextureKey(fetch);
      if (auto it = guest_textures_.find(out->key); it != guest_textures_.end() && it->second) {
        out->w = it->second->width;
        out->h = it->second->height;
      }
      return out->base != 0;
    }
    return false;
  };
  // Where each draw of a frame ends up, counted without a cap. A capture showed
  // exactly one draw reaching the GPU per frame while hundreds were queued,
  // and nothing in the log said why - every early return below was either
  // silent or capped at a couple of dozen reports for the whole run.
  struct EncodeFate {
    std::atomic<uint64_t> entered{0}, invalid{0}, full{0}, no_pipeline{0}, no_vertices{0},
        drawn{0}, d3d_entered{0}, d3d_drawn{0};
  };
  static EncodeFate fate;
  {
    static std::atomic<uint64_t> last_ms{0};
    const uint64_t now = CoarseMs();
    uint64_t was = last_ms.load(std::memory_order_relaxed);
    if (now - was >= 3000 && last_ms.compare_exchange_strong(was, now)) {
      REXLOG_INFO("plume: encode fate: entered {} (Direct3D {}), invalid {}, buffers full {}, "
                  "no pipeline {}, no vertices {}, drawn {} (Direct3D {})",
                  fate.entered.load(), fate.d3d_entered.load(), fate.invalid.load(),
                  fate.full.load(), fate.no_pipeline.load(), fate.no_vertices.load(),
                  fate.drawn.load(), fate.d3d_drawn.load());
    }
  }
  StartEncodeWatchdog();
  frame_output_dest_ = 0;
  frame_resolved_dests_.clear();
  last_resolve_dest_ = 0;
  last_front_buffer_resolve_ = 0;
  frame_cleared_after_resolve_ = false;
  frame_cleared_whole_ = false;
  frame_draws_after_resolve_ = 0;
  frame_indices_after_resolve_ = 0;
  // The title reverses depth in its viewport, not its projection: zscale -1
  // and zoffset 1, so the near plane lands at 1 and the far one at 0 - which
  // is why it clears depth to 0 and tests GEQUAL. Drawn with the usual 0..1
  // range, near stayed small and GEQUAL let whatever was farther win: the
  // garage walls, drawn after the car, painted straight over it. Direct3D
  // draws get the flipped range, as the console gives them; the interface
  // keeps the ordinary one.
  bool viewport_flipped = false;
  int32_t scissor_now[4] = {0, 0, int32_t(width), int32_t(height)};
  uint32_t viewport_x = 0;
  uint32_t viewport_y = 0;
  uint32_t viewport_w = width;
  uint32_t viewport_h = height;
  // A screen wider than 16:9 (XERENGE_ASPECT_16_9 unset): the scene is drawn
  // across all of it - widescreen.cpp widens the cameras to match - and the
  // interface keeps the console's 16:9 box in the middle, or it would stretch.
  static const bool widen_scene = std::getenv("XERENGE_ASPECT_16_9") == nullptr;
  const uint32_t box_w = widen_scene && uint64_t(width) * 9 > uint64_t(height) * 16 + height
                             ? std::min(width, (height * 16 / 9) & ~1u)
                             : width;
  const uint32_t box_x = (width - box_w) / 2;
  // And narrower than 16:9 (16:10): the cameras show more above and below
  // (widescreen.cpp), and the interface keeps its 16:9 strip across the middle.
  const uint32_t box_h = widen_scene && uint64_t(height) * 16 > uint64_t(width) * 9 + width
                             ? std::min(height, (width * 9 / 16) & ~1u)
                             : height;
  const uint32_t box_y = (height - box_h) / 2;
  // Where the current 2D object's box goes: against the screen's left or right
  // edge for an object on that side of the 16:9 layout, centred otherwise -
  // decided by the object's first draw with a known extent, kept for the rest.
  uint32_t anchor_object = 0;
  uint32_t anchor_x = box_x;
  // Draws encoded since the last copy (for the debug-mode pass timings).
  uint32_t segment_draws = 0;
  // ... and the vertices those draws shade, and how often the pipeline changes.
  uint32_t segment_vertices = 0;
  uint32_t segment_binds = 0;
  // Which of the frame's passes the draws go into (for the fragment counts).
  uint32_t segment_index = 0;
  // A render target smaller than the frame - a cube face, a post-processing
  // buffer - is drawn at its own size, in the corner of the one buffer, the
  // way it occupies a corner of EDRAM on the console; its resolve copies just
  // that corner. Drawn over the whole buffer it cost a full-resolution pass
  // for a 128x128 face, and a cube face stretched to 16:10 is not square.
  static const bool targets_followed = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  auto native_extent = [&](uint32_t tw, uint32_t th, uint32_t& vw, uint32_t& vh) {
    vw = width;
    vh = height;
    // Narrower than the frame, that is: the tiled scene's target is 1280x256 -
    // one tile's band - and is still the whole frame, drawn tile by tile.
    if (targets_followed && tw != 0 && th != 0 && tw < 1280) {
      vw = std::min(tw, width);
      vh = std::min(th, height);
    }
  };
  auto use_viewport = [&](bool flipped, uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh) {
    if (flipped == viewport_flipped && vx == viewport_x && vy == viewport_y && vw == viewport_w &&
        vh == viewport_h) {
      return;
    }
    viewport_flipped = flipped;
    viewport_x = vx;
    viewport_y = vy;
    viewport_w = vw;
    viewport_h = vh;
    list->setViewports(plume::RenderViewport(float(vx), float(vy), float(vw), float(vh),
                                             flipped ? 1.0f : 0.0f, flipped ? 0.0f : 1.0f));
    list->setScissors(
        plume::RenderRect(int32_t(vx), int32_t(vy), int32_t(vx + vw), int32_t(vy + vh)));
    scissor_now[0] = int32_t(vx);
    scissor_now[1] = int32_t(vy);
    scissor_now[2] = int32_t(vx + vw);
    scissor_now[3] = int32_t(vy + vh);
  };
  // What is bound, so that a draw repeating it does not bind it again. A
  // pass break (around a copy) forgets both.
  plume::RenderPipeline* bound_pipeline = nullptr;
  // The last draw's slots and set, and what is bound: neighbouring draws often
  // share textures, and then the set is neither looked up nor bound again.
  DrawBindings last_bindings;
  plume::RenderDescriptorSet* last_draw_set = nullptr;
  uint64_t last_tables_changes = ~0ull;
  plume::RenderDescriptorSet* bound_draw_set = nullptr;
  uint32_t bound_offsets[3] = {~0u, ~0u, ~0u};
  const auto tables_changes = [&] {
    const auto changes = [](const std::unique_ptr<BindlessTable>& t) { return t ? uint64_t(t->changes()) : 0; };
    return changes(texture_set_) + changes(volume_set_) + changes(cube_set_) + changes(sampler_set_);
  };
  int bound_vertex_buffer = 0;  // 1 the frame's buffer, 2 the vertex cache
  // The constant blocks the last draw wrote, for reuse by the next.
  const uint32_t* last_vs = nullptr;
  const uint32_t* last_ps = nullptr;
  uint32_t last_vs_slot = 0;
  uint32_t last_ps_slot = 0;
  alignas(16) uint8_t last_shared[kSharedConstantBytes];
  bool have_last_shared = false;
  uint32_t last_shared_slot = 0;
  auto encode_one = [&](const GuestDrawSnapshot& snap) {
    EncodeStage("draw setup", &snap);
    ++fate.entered;
    if (snap.d3d_vertex_buffer != 0) {
      ++fate.d3d_entered;
    }
    static uint32_t skip_logs = 0;
    auto note_skip = [&](const char* why) {
      if (skip_logs < 24) {
        ++skip_logs;
        REXLOG_INFO("plume: draw skipped ({}) vs={:016X} ps={:016X} prim={} indices={}", why,
                    snap.vs_hash, snap.ps_hash, snap.prim_type, snap.num_indices);
      }
    };
    if (!snap.valid || snap.vs_hash == 0 || snap.ps_hash == 0 || snap.num_indices == 0) {
      ++fate.invalid;
      note_skip("invalid snapshot");
      return;
    }
    if (static_cast<PrimitiveType>(snap.prim_type) == PrimitiveType::kPointList &&
        snap.num_indices < 3) {
      note_skip("degenerate point list");
      return;
    }
    if (vb_used >= vb_base + kVertsPerFrame || encoded >= kSlotsPerFrame) {
      ++fate.full;
      // Dropping the rest of the frame is not something to do quietly - it
      // costs whole strings and panels on screen, and looks like a shading
      // bug rather than a capacity one.
      static uint32_t overflow_logs = 0;
      if (overflow_logs < 8) {
        ++overflow_logs;
        REXLOG_WARN("plume: draw dropped, buffers full (verts {}/{}, draws {}/{})", vb_used - vb_base,
                    kVertsPerFrame, encoded, kSlotsPerFrame);
      }
      return;
    }
    const plume::RenderPrimitiveTopology topology = MapTopology(snap.prim_type);
    // One line per distinct guest primitive type. The per-draw trace is capped
    // at a handful of lines, which is not enough to tell which types a screen
    // actually uses - and a type mapped onto the wrong host topology draws
    // recognisable geometry in the wrong shape rather than failing outright.
    static uint32_t seen_prims = 0;
    if (snap.prim_type < 32 && (seen_prims & (1u << snap.prim_type)) == 0) {
      seen_prims |= 1u << snap.prim_type;
      REXLOG_INFO("plume: first draw with prim={} -> topo={} indices={}", snap.prim_type,
                  uint32_t(topology), snap.num_indices);
    }
    RecordShaderCoverage(snap.vs_hash, snap.ps_hash);
    // The passthrough shader hands vertex positions straight to the rasteriser.
    // That is right for screen-space UI and useless for a model: its vertices
    // are in object space, and only the title's own vertex shader holds the
    // matrix that projects them. So geometry that relies on the depth test -
    // which UI never does - is drawn with the translated shaders when both are
    // available, and everything else stays on the passthrough path the
    // interface already renders correctly through.
    // The decoded video frame, drawn into the title's own target to be copied
    // out, is opaque: the menu then draws that copy blended by its alpha. With
    // the register state the snapshot happened to carry, alpha stayed at the
    // clear's zero and the menu drew the video invisible - black.
    const bool opaque_video = targets_followed && snap.has_video_frame();
    const uint32_t blend_control =
        opaque_video ? 0x00010001u
                     : (snap.blend_control ? snap.blend_control : kDefaultBlendControl);
    const uint32_t draw_color_mask = opaque_video ? 0xFu : snap.color_mask;
    // One line per distinct depth state seen. Whether a draw wants the depth
    // test is what separates the scene from the interface here, so it matters
    // a great deal whether that register is arriving at all, or arriving with
    // the test switched off.
    {
      static std::vector<uint32_t> seen_depth;
      if (seen_depth.size() < 12 &&
          std::find(seen_depth.begin(), seen_depth.end(), snap.depth_control) ==
              seen_depth.end()) {
        seen_depth.push_back(snap.depth_control);
        const GuestDepthState seen = DepthStateFromGuest(snap.depth_control);
        // PA_CL_VTE_CNTL goes with it. Its VTX_XY_FMT bit says whether the
        // vertices arrive already divided by W - that is, already in screen
        // space, which is what the interface submits and what geometry needing
        // the title's own projection does not.
        REXLOG_INFO(
            "plume: RB_DEPTHCONTROL {:08X} test {} write {} | vte {:08X} xy_pre_divided {} "
            "vs={:016X}",
            snap.depth_control, seen.enabled ? "on" : "off", seen.write ? "on" : "off",
            snap.vte_cntl, ((snap.vte_cntl >> 8) & 1) ? "yes" : "no", snap.vs_hash);
      }
    }
    // Off by default. Routing draws to the title's own shaders is the last
    // step towards a 3D scene, but it changes how geometry reaches the screen,
    // and picking the wrong draws for it takes the interface or the video with
    // it. Opt in with XERENGE_REAL_SHADERS while that is still being settled.
    static const bool real_shaders_enabled = std::getenv("XERENGE_REAL_SHADERS") != nullptr;
    // Either half of the depth state counts: this title's geometry runs with
    // the test off and the write on ("always pass, but record depth"), so
    // keying on the test alone matched nothing. Video is excluded outright -
    // it carries depth state too, yet its quad is already in screen space and
    // the passthrough path is what places it correctly.
    const GuestDepthState draw_depth = DepthStateFromGuest(snap.depth_control);
    const bool is_video = snap.has_video_frame() || IsGuestVideoBlit(snap);
    // A draw that arrived through the Direct3D calls is scene geometry by
    // construction and wants the title's own shaders whatever the depth
    // registers say. Those registers are filled by the command ring, which
    // these draws never pass through, so for them they hold the interface's
    // state - depth off - and every one fell through to the passthrough
    // pipeline, which is why correct world-space positions still reached the
    // screen untransformed.
    // Indexed Direct3D draws are the scene and always want the title's shaders.
    // The interface's inline draws are decided the way the ring's always were,
    // by their own depth state.
    const bool from_d3d = snap.d3d_vertex_buffer != 0 && snap.d3d_index_buffer != 0;
    // So are the full-screen passes that read back a copied target: the
    // post-processing and the final composite of the 3D frame. Through the
    // passthrough shader they sampled the scene and threw every pixel away -
    // it discards where alpha is zero, which is all of the copied scene - so
    // the frame under the interface came out black.
    const bool reads_copy = ReadsResolvedCopy(snap);
    // What a copy-reading pass is given: its pixel constants and textures. The
    // composite of the 3D frame came out as the colour table's corner - black -
    // everywhere, so its input colour is zero; this says whether from the
    // constants or from the textures.
    if (reads_copy) {
      static std::mutex shown_mutex;
      static std::map<uint64_t, uint32_t> shown;
      bool show = false;
      {
        std::lock_guard lock(shown_mutex);
        show = shown.size() < 8 && shown[snap.ps_hash]++ == 0;
      }
      if (show) {
        const float* pc = reinterpret_cast<const float*>(snap.ps_constants.data());
        const size_t regs = snap.ps_constants.size() * sizeof(snap.ps_constants[0]) / 16;
        std::string consts;
        for (size_t r = 0; r < regs; ++r) {
          const float* c = pc + r * 4;
          if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) {
            consts += fmt::format(" c{}=({:.3f},{:.3f},{:.3f},{:.3f})", r, c[0], c[1], c[2], c[3]);
          }
        }
        std::string slots;
        for (uint32_t slot = 0; slot < 16; ++slot) {
          const auto fetch = TextureFetchAt(snap, slot);
          if (fetch.base_address == 0) {
            continue;
          }
          slots += fmt::format(" t{}={:08X}/type{}/dim{}/fmt{}/{}x{}x{}/tiled{}{}->{}", slot,
                               fetch.base_address << 12, uint32_t(fetch.type),
                               uint32_t(fetch.dimension), uint32_t(fetch.format),
                               fetch.dimension == rex::graphics::xenos::DataDimension::k3D
                                   ? fetch.size_3d.width + 1
                                   : fetch.size_2d.width + 1,
                               fetch.dimension == rex::graphics::xenos::DataDimension::k3D
                                   ? fetch.size_3d.height + 1
                                   : fetch.size_2d.height + 1,
                               fetch.dimension == rex::graphics::xenos::DataDimension::k3D
                                   ? fetch.size_3d.depth + 1
                                   : 1u,
                               uint32_t(fetch.tiled),
                               resolved_targets_.count(fetch.base_address << 12) ? "(copy)" : "",
                               BindlessForSlot(snap, slot));
        }
        REXLOG_INFO("plume: copy-reading pass vs={:016X} ps={:016X} into {}x{}:{} |{}",
                    snap.vs_hash, snap.ps_hash, snap.d3d_target_width, snap.d3d_target_height,
                    slots, consts.empty() ? " no constants" : consts);
      }
    }
    const bool wants_depth = real_shaders_enabled && !is_video &&
                             (draw_depth.enabled || draw_depth.write || from_d3d || reads_copy);
    plume::RenderPipeline* pipeline = nullptr;
    bool passthrough = true;
    if (wants_depth) {
      EncodeStage("building the pipeline");
      pipeline = GetOrCreatePipeline(snap.vs_hash, snap.ps_hash, topology, blend_control,
                                     snap.rt1_bound ? snap.color_mask
                                                    : (snap.color_mask & 0xFu),
                                     snap.depth_control, snap.alpha_test, snap.poly_offset, snap.cull);
      passthrough = pipeline == nullptr;
    }
    {
      static std::atomic<uint64_t> real_d3d{0};
      static std::atomic<uint64_t> passthrough_d3d{0};
      static std::atomic<uint64_t> last_ms{0};
      if (from_d3d) {
        (pipeline ? real_d3d : passthrough_d3d).fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint64_t> shown{0};
        if (shown.fetch_add(1, std::memory_order_relaxed) < 4) {
          // What the scene is actually sampling. Geometry that transforms
          // correctly still comes out black if the descriptors it reads are
          // not textures, so the descriptors themselves have to be seen.
          std::string fetches;
          for (uint32_t slot = 0; slot < 4; ++slot) {
            const auto fetch = TextureFetchAt(snap, slot);
            fetches += fmt::format(" [{}]base={:08X} fmt={} {}x{}", slot, fetch.base_address,
                                   uint32_t(fetch.format), fetch.size_2d.width + 1,
                                   fetch.size_2d.height + 1);
          }
          REXLOG_INFO("plume: scene textures:{}", fetches);
          REXLOG_INFO(
              "plume: scene state depth={:08X} (test {} write {} func {}) blend={:08X} "
              "colormask={:08X} -> write mask {:X}, viewport scale=({:.1f},{:.1f},{:.3f}) "
              "offset=({:.1f},{:.1f},{:.3f})",
              snap.depth_control, draw_depth.enabled, draw_depth.write,
              uint32_t(draw_depth.function), blend_control, snap.color_mask,
              WriteMaskForRt0(snap.color_mask), snap.vport_xscale, snap.vport_yscale,
              snap.vport_zscale, snap.vport_xoffset, snap.vport_yoffset, snap.vport_zoffset);
        }
      }
      const uint64_t now = CoarseMs();
      uint64_t was = last_ms.load(std::memory_order_relaxed);
      if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
        REXLOG_INFO("plume: Direct3D draws: {} with the title's shaders, {} passthrough",
                    real_d3d.load(std::memory_order_relaxed),
                    passthrough_d3d.load(std::memory_order_relaxed));
      }
    }
    // XERENGE_VIDEO_GPU: from stage 1 the video pipeline is built for a video
    // frame's state, and from stage 3 it draws the frame.
    bool video_pipeline = false;
    if (!pipeline && snap.has_video_frame() && VideoGpuStage() >= 1) {
      plume::RenderPipeline* video =
          VideoPipelineFor(topology, blend_control, draw_color_mask, snap.depth_control);
      if (UsesVideoPipeline(snap)) {
        pipeline = video;
        video_pipeline = video != nullptr;
      }
    }
    if (!pipeline) {
      pipeline = PassthroughFor(topology, blend_control, draw_color_mask, snap.depth_control);
    }
    if (!pipeline) {
      ++fate.no_pipeline;
      note_skip("no pipeline for topology/blend");
      return;
    }
    // The layout this pipeline reads its vertices in: packed for the title's own
    // pipelines, the full one for every other.
    if (auto layout = pipeline_layouts_.find(pipeline); layout != pipeline_layouts_.end()) {
      fill_layout_mask_ = layout->second;
    } else {
      fill_layout_mask_ = kFullLayoutMask;
    }
    fill_vs_hash_ = snap.vs_hash;
    last_packed_dst_ = nullptr;
    static uint32_t real_logs = 0;
    if (!passthrough && real_logs < 8) {
      ++real_logs;
      REXLOG_INFO("plume: drawing with the title's own shaders vs={:016X} ps={:016X} prim={}",
                  snap.vs_hash, snap.ps_hash, snap.prim_type);
    }
    // A white text pass used to be dropped here whenever a coloured pass
    // existed on the same atlas. That only ever made sense while black tint
    // constants were being rejected and repainted white, which turned the
    // outline pass into a white halo around the fill. The tint slot is now
    // read directly (see ResolveUiTint), so the outline stays black and the
    // white pass is the real fill - dropping it just deleted the text.
    // Unpacking vertices one at a time on the processor. A scene draw carries
    // thousands of them and a scene frame has hundreds of draws, so this is
    // the first place to look for the two seconds a frame costs.
    const auto fill_started = PlumeTiming() ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    EncodeStage("unpacking vertices");
    const uint32_t vertex_count = FillVertices(snap, memory, vb_used, passthrough);
    {
      static std::atomic<uint64_t> total_us{0};
      static std::atomic<uint64_t> verts{0};
      static std::atomic<uint64_t> last_ms{0};
      if (PlumeTiming()) {
        total_us.fetch_add(static_cast<uint64_t>(
                               std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - fill_started)
                                   .count()),
                           std::memory_order_relaxed);
      }
      verts.fetch_add(vertex_count, std::memory_order_relaxed);
      const uint64_t now = CoarseMs();
      uint64_t was = last_ms.load(std::memory_order_relaxed);
      if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
        REXLOG_INFO("plume: vertex cache {} hits / {} misses, {} vertices in {} meshes",
                    cache_hits_, cache_misses_, cache_used_, mesh_cache_.size());
        REXLOG_INFO("plume: unpacking vertices took {} ms for {} vertices (setup+clear {} ms, "
                    "vertex loop {} ms, after {} ms, copy out {} ms)",
                    total_us.load(std::memory_order_relaxed) / 1000,
                    verts.load(std::memory_order_relaxed), g_fill_ns[0].load() / 1000000,
                    g_fill_ns[1].load() / 1000000, g_fill_ns[2].load() / 1000000,
                    g_fill_ns[3].load() / 1000000);
      }
    }
    if (vertex_count == 0) {
      ++fate.no_vertices;
      // Which draws these are, once per shader pair: a whole kind of geometry
      // (a shadow, a decal) can be lost here without anything else saying so.
      {
        static std::mutex seen_mutex;
        static std::set<std::pair<uint64_t, uint64_t>> seen;
        std::lock_guard lock(seen_mutex);
        if (seen.size() < 40 && seen.emplace(snap.vs_hash, snap.ps_hash).second) {
          REXLOG_WARN("plume: draw has no vertices: vs={:016X} ps={:016X} prim {} indices {} "
                      "vb {:08X}+{} ib {:08X} d3d {} target {}x{}",
                      snap.vs_hash, snap.ps_hash, snap.prim_type, snap.num_indices,
                      snap.d3d_vertex_buffer, snap.d3d_vertex_stride, snap.d3d_index_buffer,
                      snap.d3d_vertex_buffer != 0 ? 1 : 0, snap.d3d_target_width,
                      snap.d3d_target_height);
        }
      }
      return;
    }

    // Each block goes into this draw's slot unless it is the same as the one
    // the last draw wrote, which is then used again: neighbouring draws often
    // share their pixel constants, and every block written is 4 KB going into
    // memory the GPU reads.
    uint32_t vs_slot = slot_base + encoded;
    uint32_t ps_slot = slot_base + encoded;
    uint32_t shared_slot = slot_base + encoded;
    // Snapshots share their constant words (SharedWords), and the title's
    // thread already gives a draw the last one's block when the words are the
    // same (AssignShared): another block is all but always different, and
    // comparing its 4 KB to find that out cost more than writing it.
    if (last_vs && last_vs == snap.vs_constants.data()) {
      vs_slot = last_vs_slot;
    } else {
      std::memcpy(static_cast<uint8_t*>(vs_constants_mapped_) + vs_slot * kVsSlotBytes,
                  snap.vs_constants.data(), kVsConstantBytes);
      last_vs = snap.vs_constants.data();
      last_vs_slot = vs_slot;
    }
    if (last_ps && last_ps == snap.ps_constants.data()) {
      ps_slot = last_ps_slot;
    } else {
      std::memcpy(static_cast<uint8_t*>(ps_constants_mapped_) + ps_slot * kPsSlotBytes,
                  snap.ps_constants.data(), kPsConstantBytes);
      last_ps = snap.ps_constants.data();
      last_ps_slot = ps_slot;
    }
    EncodeStage("binding textures (creating and uploading them)");
    alignas(16) uint8_t shared[kSharedConstantBytes];
    FillSharedConstants(shared, snap, width, height);
    if (have_last_shared && std::memcmp(last_shared, shared, kSharedConstantBytes) == 0) {
      shared_slot = last_shared_slot;
    } else {
      std::memcpy(static_cast<uint8_t*>(shared_constants_mapped_) + shared_slot * kSharedSlotBytes,
                  shared, kSharedConstantBytes);
      std::memcpy(last_shared, shared, kSharedConstantBytes);
      have_last_shared = true;
      last_shared_slot = shared_slot;
    }
    EncodeStage("recording the draw");

    const uint32_t constant_offsets[3] = {vs_slot * kVsSlotBytes, ps_slot * kPsSlotBytes,
                                          shared_slot * kSharedSlotBytes};
    // The draw's sampler slots. The title's shaders read slot n for their sampler
    // register n (the indices FillSharedConstants worked out); the passthrough
    // shader reads its one texture from slot 0, the video shader its planes from
    // slots 0-2.
    DrawBindings bindings;
    if (video_pipeline) {
      const uint32_t u = VideoPlaneSlot(snap.video_u_key);
      bindings.tex2d[0] = VideoPlaneSlot(snap.video_key);
      bindings.tex2d[1] = u;
      bindings.tex2d[2] = snap.video_packed_uv ? u : VideoPlaneSlot(snap.video_v_key);
    } else if (passthrough) {
      bindings.tex2d[0] = BindlessForTexture(snap);
    } else {
      const auto* words = reinterpret_cast<const uint32_t*>(shared);
      for (uint32_t s = 0; s < kDrawSlots; ++s) {
        bindings.tex2d[s] = words[s];
        bindings.layered[s] = words[16 + s];
        bindings.cube[s] = words[32 + s];
        bindings.sampler[s] = words[48 + s];
      }
    }
    const uint64_t changes_now = tables_changes();
    plume::RenderDescriptorSet* draw_set = nullptr;
    if (last_draw_set && changes_now == last_tables_changes &&
        std::memcmp(&bindings, &last_bindings, sizeof(bindings)) == 0) {
      draw_set = last_draw_set;
    } else {
      draw_set = DrawSetFor(bindings);
      last_bindings = bindings;
      last_draw_set = draw_set;
      last_tables_changes = changes_now;
    }
    if (!draw_set) {
      note_skip("no descriptor set");
      return;
    }
    if (draw_set != bound_draw_set || std::memcmp(constant_offsets, bound_offsets, sizeof(bound_offsets)) != 0) {
      list->setGraphicsDescriptorSetDynamic(draw_set, 0, constant_offsets, 3);
      bound_draw_set = draw_set;
      std::memcpy(bound_offsets, constant_offsets, sizeof(bound_offsets));
    }
    if (pipeline != bound_pipeline) {
      list->setPipeline(pipeline);
      bound_pipeline = pipeline;
      ++segment_binds;
    }
    if (!passthrough && snap.poly_offset) {
      // Xenos keeps the slope term in 1/16 subpixel units and the constant in
      // depth range units; Vulkan wants the slope as is and the constant in
      // steps of the depth format - 2^-24 around the near end, where the
      // reversed depth of the road under the car sits.
      list->setDepthBias(snap.poly_offset_offset * 16777216.0f, 0.0f,
                         snap.poly_offset_scale * (1.0f / 16.0f));
    }
    {
      uint32_t vw = width;
      uint32_t vh = height;
      if (snap.d3d_vertex_buffer != 0) {
        native_extent(snap.d3d_target_width, snap.d3d_target_height, vw, vh);
      }
      // The interface - marked by the title's 2D layer, or from the ring (the
      // menus) - in the 16:9 box when the frame is wider. And the video by what
      // it carries: it comes through either path from frame to frame, and
      // drawn by the other one it jumped between the box and the whole width.
      const bool boxed = (box_w != width || box_h != height) && vw == width && vh == height &&
                         (snap.interface_draw || snap.d3d_vertex_buffer == 0 ||
                          snap.has_video_frame());
      // The music player's panel keeps its box against the screen's left edge;
      // the HUD's objects go to the edge of their side, each one whole.
      uint32_t vx = 0;
      uint32_t vy = 0;
      if (boxed) {
        vx = box_x;
        vy = box_y;
        if (snap.interface_left) {
          vx = 0;
        } else if (snap.interface_object != 0 && !snap.has_video_frame()) {
          if (snap.interface_object != anchor_object && last_fill_x_known_) {
            anchor_object = snap.interface_object;
            const float guest_w =
                snap.d3d_target_width != 0 ? float(snap.d3d_target_width) : 1280.0f;
            const float lo = last_fill_x_[0] / guest_w;
            const float hi = last_fill_x_[1] / guest_w;
            const float mid = (lo + hi) * 0.5f;
            anchor_x = hi - lo > 0.6f ? box_x
                       : mid < 0.4f   ? 0
                       : mid > 0.6f   ? width - box_w
                                      : box_x;
          }
          if (snap.interface_object == anchor_object) {
            vx = anchor_x;
          }
        }
      }
      if (boxed) {
        vw = box_w;
        vh = box_h;
      }
      use_viewport(snap.d3d_vertex_buffer != 0, vx, vy, vw, vh);
      // The title's scissor, scaled from its render target's pixels to the
      // area that target is drawn over here.
      int32_t sx0 = int32_t(vx), sy0 = int32_t(vy), sx1 = int32_t(vx + vw), sy1 = int32_t(vy + vh);
      if (snap.scissor_enabled && snap.d3d_target_width != 0 && snap.d3d_target_height != 0) {
        const float kx = float(vw) / float(snap.d3d_target_width);
        const float ky = float(vh) / float(snap.d3d_target_height);
        sx0 = std::clamp(int32_t(vx) + int32_t(std::floor(snap.scissor_rect[0] * kx)),
                         int32_t(vx), int32_t(vx + vw));
        sy0 = std::clamp(int32_t(vy) + int32_t(std::floor(snap.scissor_rect[1] * ky)), int32_t(vy),
                         int32_t(vy + vh));
        sx1 = std::clamp(int32_t(vx) + int32_t(std::ceil(snap.scissor_rect[2] * kx)), sx0,
                         int32_t(vx + vw));
        sy1 = std::clamp(int32_t(vy) + int32_t(std::ceil(snap.scissor_rect[3] * ky)), sy0,
                         int32_t(vy + vh));
        // Nothing left of it: the takedown camera's whole scene (511 draws and
        // the blur composite) came out against an empty scissor, and the replay
        // was the clear colour alone. The title would not draw a scene to clip
        // all of it away; the rectangle at +0x3220 is not what the GPU used
        // there (Direct3D intersects it with the viewport into
        // PA_SC_WINDOW_SCISSOR, and the tiled scene's band is only 256 high), so
        // an empty result is taken as no scissor rather than as nothing at all.
        if (sx1 <= sx0 || sy1 <= sy0) {
          static std::atomic<uint32_t> shown{0};
          if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
            REXLOG_INFO("plume: the title's scissor ({},{},{},{}) on a {}x{} target is empty here; "
                        "drawn without it (vs={:016X})",
                        snap.scissor_rect[0], snap.scissor_rect[1], snap.scissor_rect[2],
                        snap.scissor_rect[3], snap.d3d_target_width, snap.d3d_target_height,
                        snap.vs_hash);
          }
          sx0 = int32_t(vx);
          sy0 = int32_t(vy);
          sx1 = int32_t(vx + vw);
          sy1 = int32_t(vy + vh);
        }
      }
      if (sx0 != scissor_now[0] || sy0 != scissor_now[1] || sx1 != scissor_now[2] ||
          sy1 != scissor_now[3]) {
        list->setScissors(plume::RenderRect(sx0, sy0, sx1, sy1));
        scissor_now[0] = sx0;
        scissor_now[1] = sy0;
        scissor_now[2] = sx1;
        scissor_now[3] = sy1;
      }
    }
    // Debug mode: the fragments this draw shades, counted on the GPU.
    uint32_t fragment_query = ~0u;
    if (pass && pass->begin_fragment_count) {
      FragmentCountLabel label;
      label.vs_hash = snap.vs_hash;
      label.ps_hash = snap.ps_hash;
      label.pass = segment_index;
      label.vertices = vertex_count;
      label.depth_control = snap.depth_control;
      label.blend_control = snap.blend_control;
      label.target_width = snap.d3d_target_width;
      label.target_height = snap.d3d_target_height;
      fragment_query = pass->begin_fragment_count(pass->context, label);
    }
    if (fill_layout_mask_ != kFullLayoutMask) {
      // Packed: the draw's vertices start at the front of its room, and the
      // stride is the pipeline's, so the buffer is bound at that room.
      const bool cached = last_fill_cache_offset_ != ~0u;
      const uint32_t first = cached ? last_fill_cache_offset_ : vb_used;
      const plume::RenderVertexBufferView view(
          plume::RenderBufferReference(cached ? cache_vb_.get() : dummy_vb_.get(),
                                       uint64_t(first) * kVertexStrideBytes),
          (cached ? kCacheVertexCount : kDummyVertexCount) * kVertexStrideBytes -
              first * kVertexStrideBytes);
      list->setVertexBuffers(0, &view, 1, &input_slots_[0]);
      bound_vertex_buffer = 0;
      static const bool pack_check = std::getenv("XERENGE_PACK_CHECK") != nullptr;
      if (pack_check && last_packed_dst_) {
        const float* bound = (cached ? cache_mapped_ : vb_mapped_) + size_t(first) * kFloatsPerVert;
        if (bound != last_packed_dst_) {
          static std::atomic<uint32_t> shown{0};
          if (shown.fetch_add(1, std::memory_order_relaxed) < 16) {
            REXLOG_WARN("plume: pack check: draw bound at {} +{} but its vertices went to +{} "
                        "(vs={:016X})",
                        cached ? "cache" : "frame buffer",
                        size_t(bound - (cached ? cache_mapped_ : vb_mapped_)),
                        size_t(last_packed_dst_ - (cached ? cache_mapped_ : vb_mapped_)),
                        snap.vs_hash);
          }
        }
      }
      list->drawInstanced(vertex_count, 1, 0, 0);
      if (!cached) {
        vb_used += vertex_count;
      }
    } else if (last_fill_cache_offset_ != ~0u) {
      if (bound_vertex_buffer != 2) {
        list->setVertexBuffers(0, &cache_view_, 1, &input_slots_[0]);
        bound_vertex_buffer = 2;
      }
      list->drawInstanced(vertex_count, 1, last_fill_cache_offset_, 0);
    } else {
      if (bound_vertex_buffer != 1) {
        list->setVertexBuffers(0, &vb_view_, 1, &input_slots_[0]);
        bound_vertex_buffer = 1;
      }
      list->drawInstanced(vertex_count, 1, vb_used, 0);
      vb_used += vertex_count;
    }
    if (fragment_query != ~0u) {
      pass->end_fragment_count(pass->context, fragment_query);
    }
    ++encoded;
    segment_vertices += vertex_count;
    ++fate.drawn;
    if (snap.d3d_vertex_buffer != 0) {
      ++fate.d3d_drawn;
    }
  };
  // What each pass need not load or store. Every pass loaded and stored all
  // three attachments over its area - colour, the second target, depth - and
  // on a tiled GPU that is the area through memory six times: a pass of one
  // draw over the frame took 1.2 ms on the Xperia. Now a pass loads an
  // attachment only when something reads it before it is cleared, and keeps
  // one only when something reads it - in the pass or later in the frame.
  // Colour is shown, so always kept; depth and the second target start the
  // next frame cleared.
  // XERENGE_LOAD_EVERYTHING: the old way, for comparison.
  static const bool access_wanted = std::getenv("XERENGE_LOAD_EVERYTHING") == nullptr;
  const bool track_access = access_wanted && targets_followed && pass != nullptr;
  constexpr uint32_t kColourBit = 1u;
  constexpr uint32_t kSecondBit = 2u;
  constexpr uint32_t kDepthBit = plume::RenderDepthAttachmentBit;
  const uint32_t all_attachments = kColourBit |
                                   (pass && pass->second_target ? kSecondBit : 0u) |
                                   (pass && pass->has_depth ? kDepthBit : 0u);
  // What a draw reads from the attachments before it writes them: colour
  // always (blending, or the pixels it leaves), depth when it tests against
  // it. A test switched off neither reads nor writes depth (Vulkan writes
  // depth only with the test on), and ALWAYS reads nothing. The second
  // target is only ever written, unblended.
  auto draw_reads = [&](const GuestDrawSnapshot& d) -> uint32_t {
    uint32_t reads = kColourBit;
    const GuestDepthState depth = DepthStateFromGuest(d.depth_control);
    if (depth.enabled && depth.function != plume::RenderComparisonFunction::ALWAYS) {
      reads |= kDepthBit;
    }
    return reads & all_attachments;
  };
  // What a clear sets everywhere in an area_w x area_h pass (see where clears
  // are made below: a corner, the whole target, or - a smaller target whose
  // size reaches the frame's - nothing).
  auto clear_covers = [&](const GuestDrawSnapshot& c, uint32_t area_w,
                          uint32_t area_h) -> uint32_t {
    uint32_t covers = 0;
    if (c.clear_width != 0) {
      uint32_t cw = 0;
      uint32_t ch = 0;
      native_extent(c.clear_width, c.clear_height, cw, ch);
      if ((cw < width || ch < height) && cw >= area_w && ch >= area_h) {
        covers |= (c.clear_flags & 0x10u) != 0 ? kDepthBit : 0u;
        covers |= (c.clear_flags & 0x1u) != 0 ? kColourBit : 0u;
      }
    } else {
      covers |= (c.clear_flags & 0x10u) != 0 ? kDepthBit : 0u;
      covers |= (c.clear_flags & 0x1u) != 0 && c.clear_whole ? kColourBit : 0u;
      covers |= (c.clear_flags & 0x2u) != 0 && c.clear_whole ? kSecondBit : 0u;
    }
    return covers & all_attachments;
  };
  // The pass from draws[from] up to the next copy, area_w x area_h, after the
  // frame's opening clear when `opened_cleared`.
  auto attachment_access = [&](size_t from, uint32_t area_w, uint32_t area_h,
                               bool opened_cleared, uint32_t& no_load, uint32_t& discard) {
    uint32_t read = 0;
    uint32_t read_before_clear = 0;
    uint32_t cleared = opened_cleared ? all_attachments : 0u;
    size_t j = from;
    for (; j < draws.size(); ++j) {
      if (Skipped(j)) {
        continue;
      }
      const GuestDrawSnapshot& e = draws[j];
      if (e.is_resolve) {
        break;
      }
      if (e.is_clear) {
        cleared |= clear_covers(e, area_w, area_h);
        continue;
      }
      const uint32_t reads = draw_reads(e);
      read_before_clear |= reads & ~cleared;
      read |= reads;
    }
    // Used later: read by a copy or a draw before a clear over this pass's
    // area. Colour is shown at the end of the frame.
    uint32_t live = 0;
    uint32_t dead = 0;
    for (size_t k = j; k < draws.size() && (live | dead) != all_attachments; ++k) {
      if (Skipped(k)) {
        continue;
      }
      const GuestDrawSnapshot& e = draws[k];
      if (e.is_clear) {
        // Cleared over this pass's area: what it stored there is gone.
        dead |= clear_covers(e, area_w, area_h) & ~live;
        continue;
      }
      const uint32_t uses =
          e.is_resolve ? ((e.resolve_source == 1 && (all_attachments & kSecondBit)) ? kSecondBit
                                                                                     : kColourBit)
                       : draw_reads(e);
      live |= uses & ~dead;
    }
    live |= kColourBit & ~dead;
    // Loaded when the pass reads it before clearing it, or when it is used
    // later and not cleared here - what the pass does not draw over has to
    // come through. Stored unless nothing reads it here or later: written
    // and never read, it may still be thrown away, since a pass begun again
    // after a barrier or a copy finds garbage only where nothing looks.
    no_load = all_attachments & ~(read_before_clear | (live & ~cleared));
    discard = all_attachments & ~live & ~read;
    if (xerenge::Diagnostics()) {
      LogPassAccess(from, area_w, no_load, discard);
    }
  };
  if (pass && pass->clear_first) {
    uint32_t no_load = 0;
    uint32_t discard = 0;
    if (track_access) {
      attachment_access(0, width, height, true, no_load, discard);
    }
    ClearFirstPass(list, pass, no_load, discard);
  }
  // The decoded video goes first, under everything, when the ring's draws are
  // all there is. With targets followed it keeps its place instead: the title
  // clears the target, draws the video into it and copies it out for the menu
  // to draw, so hoisted to the front it was wiped by that clear and the menu
  // drew a black copy, the video showing through only now and then.
  const size_t hoisted_video = targets_followed ? draws.size() : last_video;
  if (hoisted_video < draws.size()) {
    encode_one(draws[hoisted_video]);
  }
  // Whether anything was drawn after the frame's last copy. A frame that ends
  // copy-then-clear - the menus and the garage alike: copy into the front
  // buffer, clear the target for the next frame - is shown from that copy; a
  // frame that draws on after it is shown from the target itself.
  bool drawn_since_copy = true;
  bool video_since_clear = false;
  {
    // XERENGE_DUMP_ORDER=<frame>: that frame's queue as encoded, one line an
    // entry - which clears, copies and draws, from Direct3D or the ring.
    static const uint64_t dump_frame = [] {
      const char* v = std::getenv("XERENGE_DUMP_ORDER");
      return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    // XERENGE_DUMP_ORDER_EVERY=<seconds>: one frame's queue every so often,
    // for a screen whose frame number is not known in advance.
    static const double dump_every = [] {
      const char* v = std::getenv("XERENGE_DUMP_ORDER_EVERY");
      return v ? std::strtod(v, nullptr) : 0.0;
    }();
    bool dump_now = false;
    if (dump_every > 0.0) {
      static auto next_dump = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_dump) {
        next_dump = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(dump_every));
        dump_now = true;
      }
    }
    std::string dump_tag;
    {
      std::lock_guard lock(dump_request_mutex_);
      if (dump_requested_) {
        dump_requested_ = false;
        dump_tag = std::move(dump_request_tag_);
        dump_now = true;
      }
    }
    if (!dump_tag.empty()) {
      REXLOG_INFO("plume: the queue below goes with {}", dump_tag);
    }
    if (dump_now ||
        (dump_frame != 0 && frame_serial_ >= dump_frame && frame_serial_ < dump_frame + 8)) {
      std::string out;
      for (size_t i = 0; i < draws.size(); ++i) {
        const GuestDrawSnapshot& d = draws[i];
        if (d.is_clear) {
          out += fmt::format("\n  {} clear flags {:X} whole {} colour ({:.2f},{:.2f},{:.2f},{:.2f})",
                             i, d.clear_flags, d.clear_whole, d.clear_color[0], d.clear_color[1],
                             d.clear_color[2], d.clear_color[3]);
        } else if (d.is_resolve) {
          out += fmt::format("\n  {} copy -> {:08X} {}x{} source {}", i, d.resolve_dest,
                             d.resolve_width, d.resolve_height, d.resolve_source);
        } else {
          const auto t0 = TextureFetchAt(d, 0);
          out += fmt::format("\n  {} draw prim {} n {} vs {:016X} ps {:016X} vb {:08X} ib {:08X} "
                             "target {}x{} blend {:08X} mask {:X} at {} ref {:.3f} tex0 t{} {:08X} "
                             "fmt {} {}x{} swz {:03X} clamp {}{}{}",
                             i, d.prim_type, d.num_indices, d.vs_hash, d.ps_hash,
                             d.d3d_vertex_buffer, d.d3d_index_buffer, d.d3d_target_width,
                             d.d3d_target_height, d.blend_control, d.color_mask,
                             d.alpha_test ? 1 : 0, d.alpha_ref, uint32_t(t0.type),
                             uint32_t(t0.base_address) << 12, uint32_t(t0.format),
                             uint32_t(t0.size_2d.width) + 1, uint32_t(t0.size_2d.height) + 1,
                             uint32_t(t0.swizzle), uint32_t(t0.clamp_x), uint32_t(t0.clamp_y),
                             !d.has_video_frame() ? "" : " video");
          // A small interface draw's vertices, raw: where its quads meet and
          // which texels they reach is what a seam between them comes down to.
          const uint32_t vb = d.d3d_vertex_buffer;
          const uint32_t stride = d.d3d_vertex_stride;
          if (memory && vb != 0 && d.d3d_index_buffer == 0 && stride >= 8 && stride <= 64 &&
              d.num_indices != 0 && d.num_indices <= 48) {
            uint32_t phys = vb & 0x1FFFFFFFu;
            if (vb >= 0xE0000000u) {
              phys += 0x1000u;
            }
            const uint8_t* base = memory->TranslatePhysical<const uint8_t*>(phys);
            const uint32_t count = std::min<uint32_t>(d.num_indices, stride <= 16 ? 24u : 6u);
            const uint32_t words = std::min<uint32_t>(stride / 4, 6);
            for (uint32_t v = 0; base && v < count; ++v) {
              out += "\n      v";
              for (uint32_t w = 0; w < words; ++w) {
                uint32_t bits = 0;
                std::memcpy(&bits, base + v * stride + w * 4, 4);
                bits = __builtin_bswap32(bits);
                float value = 0.0f;
                std::memcpy(&value, &bits, 4);
                out += (std::fabs(value) < 1e6f && std::fabs(value) > 1e-6f) || bits == 0
                           ? fmt::format(" {:.4f}", value)
                           : fmt::format(" #{:08X}", bits);
              }
            }
          }
        }
      }
      REXLOG_INFO("plume: queue of frame {} ({} entries):{}", frame_serial_, draws.size(), out);
    }
  }
  // The copies made so far this frame, and how many draws and clears had
  // reached the target by then (see the repeat test where copies are made).
  struct FrameCopy {
    uint32_t dest;
    uint32_t source;
    uint32_t face;
    bool cube;
    uint64_t writes;
  };
  std::vector<FrameCopy> frame_copies;
  uint64_t target_writes = 0;
  // After a copy the pass stays closed until something is drawn or cleared:
  // copies back to back no longer open and close an empty pass between them,
  // which on a tiled GPU loaded and stored every attachment for nothing.
  bool pass_closed = false;
  auto reopen_pass = [&](size_t from) {
    pass->begin(pass->context);
    // The part of the target the draws up to the next copy touch. The
    // garage's reflection faces and the post-processing buffers are drawn in
    // a corner, a 128x128 one each face; reopened over the whole target, a
    // tiled GPU (the phones') loaded and stored all of it - colour, depth and
    // the second target, some fifteen times a frame - for that corner. Only
    // what a pass draws into is its area now; anything drawn whole, or
    // unknown, keeps the whole target.
    {
      uint32_t area_w = 0;
      uint32_t area_h = 0;
      for (size_t j = from; j < draws.size(); ++j) {
        if (j == hoisted_video || Skipped(j)) {
          continue;
        }
        const GuestDrawSnapshot& next = draws[j];
        if (next.is_resolve) {
          break;
        }
        uint32_t ew = width;
        uint32_t eh = height;
        if (next.is_clear) {
          if (next.clear_width != 0) {
            native_extent(next.clear_width, next.clear_height, ew, eh);
          }
        } else if (next.d3d_vertex_buffer != 0 && !next.has_video_frame()) {
          native_extent(next.d3d_target_width, next.d3d_target_height, ew, eh);
        }
        area_w = std::max(area_w, ew);
        area_h = std::max(area_h, eh);
        if (area_w >= width && area_h >= height) {
          break;
        }
      }
      if (area_w != 0 && (area_w < width || area_h < height)) {
        const plume::RenderRect area(0, 0, int32_t(area_w), int32_t(area_h));
        list->setRenderArea(&area);
      } else {
        area_w = width;
        area_h = height;
      }
      if (track_access) {
        uint32_t no_load = 0;
        uint32_t discard = 0;
        attachment_access(from, area_w, area_h, false, no_load, discard);
        list->setAttachmentAccess(no_load, discard);
      }
    }
  };
  for (size_t snap_index = 0; snap_index < draws.size(); ++snap_index) {
    if (snap_index == hoisted_video || Skipped(snap_index)) {
      continue;
    }
    const GuestDrawSnapshot& snap = draws[snap_index];
    if (!snap.is_resolve) {
      // Any draw or clear, made or refused, counts as a write: so the first copy
      // after the pass reopens is never taken for a repeat, which the area
      // reopen_pass picks - up to that copy - depends on.
      ++target_writes;
      if (pass_closed) {
        reopen_pass(snap_index);
        pass_closed = false;
      }
    }
    if (snap.is_clear) {
      if ((snap.clear_flags & 0x1u) != 0) {
        video_since_clear = false;
        if (snap.clear_whole) {
          frame_cleared_whole_ = true;
          if (last_resolve_dest_ != 0) {
            frame_cleared_after_resolve_ = true;
          }
        }
      }
      // A smaller target cleared whole: the corner it is drawn in, at the size
      // it is drawn at (see native_extent and the ClearF hook).
      if (snap.clear_width != 0) {
        uint32_t cw = 0;
        uint32_t ch = 0;
        native_extent(snap.clear_width, snap.clear_height, cw, ch);
        if (cw < width || ch < height) {
          const plume::RenderRect corner(0, 0, int32_t(cw), int32_t(ch));
          if ((snap.clear_flags & 0x10u) != 0) {
            list->clearDepth(true, snap.clear_depth, &corner, 1);
          }
          if ((snap.clear_flags & 0x1u) != 0) {
            list->clearColor(0, plume::RenderColor(snap.clear_color[0], snap.clear_color[1],
                                                   snap.clear_color[2], snap.clear_color[3]),
                             &corner, 1);
          }
          static std::atomic<uint32_t> shown{0};
          if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
            REXLOG_INFO("plume: clear of a {}x{} target (flags {:X}, depth {}) in its {}x{} corner",
                        snap.clear_width, snap.clear_height, snap.clear_flags, snap.clear_depth,
                        cw, ch);
          }
        }
        continue;
      }
      // Where the title cleared depth, clear it here too, to its value. This
      // stays inside the pass: clearing an attachment does not need it closed.
      if ((snap.clear_flags & 0x10u) != 0) {
        list->clearDepth(true, snap.clear_depth);
      }
      // And colour, when every target is drawn into this one buffer and the
      // clear covers the whole target - a rectangle clear is left alone rather
      // than wiping the rest of the picture.
      if ((snap.clear_flags & 0x1u) != 0 && snap.clear_whole) {
        list->clearColor(0, plume::RenderColor(snap.clear_color[0], snap.clear_color[1],
                                               snap.clear_color[2], snap.clear_color[3]));
      }
      if ((snap.clear_flags & 0x2u) != 0 && snap.clear_whole && pass &&
          pass->second_target) {
        list->clearColor(1, plume::RenderColor(snap.clear_color[0], snap.clear_color[1],
                                               snap.clear_color[2], snap.clear_color[3]));
      }
      continue;
    }
    if (snap.is_resolve) {
      // The copy belongs here, between the draws: the title copies mid-frame
      // and the draws that follow read back what it copied. Copying out of a
      // target still bound for writing is invalid, so the pass is closed
      // around it and opened again - the draws already made survive, since
      // reopening does not clear.
      // Off by default. Breaking the render pass mid-frame and copying between
      // the halves is new work for the device, and it hung one - so it stays
      // behind a flag until it has been shown to be safe, whatever it does for
      // the picture.
      static const bool copy_in_order = std::getenv("XERENGE_COPY_IN_ORDER") != nullptr ||
                                        std::getenv("XERENGE_D3D_TARGETS") != nullptr;
      if (video_since_clear) {
        video_copy_dests_[snap.resolve_dest] = frame_serial_;
      } else if (const auto vd = video_copy_dests_.find(snap.resolve_dest);
                 vd != video_copy_dests_.end() && frame_serial_ - vd->second <= 8) {
        // Skipped, and that copy - the last frame with its video - is what is
        // shown when it is the front buffer. Shown from the target instead,
        // the menus blinked between frames with and without their video.
        // No size given: the whole frame, whatever size it is drawn at.
        const uint32_t rw = snap.resolve_width ? snap.resolve_width : 1280u;
        const uint32_t rh = snap.resolve_height ? snap.resolve_height : 720u;
        if (!snap.resolve_cube && (IsKnownFrontBuffer(snap.resolve_dest) || (rw >= 1280 && rh >= 720)) &&
            resolved_targets_.count(snap.resolve_dest) != 0) {
          frame_output_dest_ = snap.resolve_dest;
          // What was drawn before it is in that copy's place now, as far as the
          // frame goes: without this the end of the frame showed the target again.
          drawn_since_copy = false;
          last_resolve_dest_ = snap.resolve_dest;
        }
        continue;
      }
      if (copy_in_order && pass && pass->end && pass->begin && colour_target) {
        // The same copy again - into the same place, from the same target,
        // with nothing drawn or cleared since - is left out. The tiled scene
        // copies each of its three bands into three textures; drawn whole
        // here, every band's copy is the whole frame: nine full-frame copies
        // where three say it all, each a third of a millisecond on the Xperia.
        bool repeat = false;
        for (const FrameCopy& copy : frame_copies) {
          if (copy.dest == snap.resolve_dest && copy.source == snap.resolve_source &&
              copy.face == snap.resolve_face && copy.cube == snap.resolve_cube) {
            repeat = copy.writes == target_writes;
            break;
          }
        }
        uint32_t region_w = 0;
        uint32_t region_h = 0;
        native_extent(snap.resolve_width, snap.resolve_height, region_w, region_h);
        // The motion vectors, with no second target to copy them from (motion
        // blur off): no motion, as the scene writes it (ResolveRenderTarget), put
        // in once. Copied from the colour
        // instead, the blur at speed read the picture as movement.
        const bool zero_vectors =
            snap.resolve_source == 1 && !pass->second_target && !snap.resolve_cube;
        if (!repeat && zero_vectors &&
            ZeroedTargetReady(snap.resolve_dest, width, height, region_w, region_h)) {
          repeat = true;
        }
        if (!repeat) {
          if (!pass_closed) {
            pass->end(pass->context);
            pass_closed = true;
          }
          plume::RenderTexture* const source =
              (snap.resolve_source == 1 && pass->second_target) ? pass->second_target
                                                                : colour_target;
          {
            // Which copies a frame makes, when that changes: the menu's video
            // came out as its own top-left corner, blown up, only with targets
            // followed.
            static std::mutex seen_mutex;
            static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen;
            std::lock_guard lock(seen_mutex);
            if (seen.size() < 200 &&
                seen.emplace(snap.resolve_dest, snap.resolve_width, snap.resolve_height, region_w)
                    .second) {
              REXLOG_INFO("plume: copy -> {:08X} {}x{} region {}x{} source {} face {} cube {}",
                          snap.resolve_dest, snap.resolve_width, snap.resolve_height, region_w,
                          region_h, snap.resolve_source, snap.resolve_face, snap.resolve_cube);
            }
          }
          ResolveRenderTarget(list, source, width, height, snap.resolve_dest,
                              snap.resolve_width ? snap.resolve_width : width,
                              snap.resolve_height ? snap.resolve_height : height, region_w,
                              region_h, snap.resolve_face, snap.resolve_cube, zero_vectors);
          if (pass->mark) {
            pass->mark(pass->context, snap.resolve_dest, segment_draws, segment_vertices,
                       segment_binds);
          }
          segment_draws = 0;
          segment_vertices = 0;
          segment_binds = 0;
          ++segment_index;
          bool known = false;
          for (FrameCopy& copy : frame_copies) {
            if (copy.dest == snap.resolve_dest && copy.source == snap.resolve_source &&
                copy.face == snap.resolve_face && copy.cube == snap.resolve_cube) {
              copy.writes = target_writes;
              known = true;
              break;
            }
          }
          if (!known) {
            frame_copies.push_back({snap.resolve_dest, snap.resolve_source, snap.resolve_face,
                                    snap.resolve_cube, target_writes});
          }
        }
        bound_pipeline = nullptr;
        bound_draw_set = nullptr;
        bound_vertex_buffer = 0;
        last_resolve_dest_ = snap.resolve_dest;
        // No size given: the whole frame, whatever size it is drawn at.
        const uint32_t rw = snap.resolve_width ? snap.resolve_width : 1280u;
        const uint32_t rh = snap.resolve_height ? snap.resolve_height : 720u;
        const bool is_front =
            snap.is_end_tiling || IsKnownFrontBuffer(snap.resolve_dest) ||
            (!snap.resolve_cube && rw >= 1280 && rh >= 720 &&
             snap.resolve_dest != 0x0E6CA000 && snap.resolve_dest != 0x0EA63000);
        if (is_front) {
          last_front_buffer_resolve_ = snap.resolve_dest;
          frame_output_dest_ = snap.resolve_dest;
        } else if (last_front_buffer_resolve_ == 0 && rw >= 1280 && rh >= 720 && !snap.resolve_cube) {
          frame_output_dest_ = snap.resolve_dest;
        }
        frame_resolved_dests_.insert(snap.resolve_dest);
        drawn_since_copy = false;
        frame_cleared_after_resolve_ = false;
        frame_draws_after_resolve_ = 0;
        frame_indices_after_resolve_ = 0;
        // Reopening the pass restores the ordinary viewport and scissor.
        viewport_flipped = false;
        scissor_now[0] = 0;
        scissor_now[1] = 0;
        scissor_now[2] = int32_t(width);
        scissor_now[3] = int32_t(height);
        viewport_x = 0;
        viewport_y = 0;
        viewport_w = width;
        viewport_h = height;
        ++resolved_in_place;
        // Reported here, not beside the frame summary: copies are far rarer
        // than presents, so a summary printed every hundred and twentieth
        // frame almost never lands on one that contains a copy - which is why
        // this looked like the markers were never arriving.
        static std::atomic<uint64_t> copies{0};
        const uint64_t n = copies.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n % 500) == 0) {
          REXLOG_INFO("plume: copy #{} made in draw order -> {:08X} ({}x{})", n, snap.resolve_dest,
                      snap.resolve_width, snap.resolve_height);
        }
      }
      continue;
    }
    // Only a draw that really went into the frame counts. The clears' own
    // rectangles arrive through the ring after the copy and are refused - had
    // they counted, every menu frame would show the cleared target: black.
    const uint32_t encoded_before = encoded;
    encode_one(snap);
    if (encoded != encoded_before) {
      ++segment_draws;
      // Only a Direct3D draw paints over the picture. What arrives after a
      // frame's last copy through the ring alone - the rectangles of the next
      // frame's clears - or the next frame's video frame belongs to the frame
      // after; counted, the target (already cleared) was shown instead of the
      // copy, and a black frame flashed up now and then.
      if (snap.d3d_vertex_buffer != 0 && !snap.has_video_frame()) {
        drawn_since_copy = true;
        if (last_resolve_dest_ != 0) {
          ++frame_draws_after_resolve_;
          frame_indices_after_resolve_ += snap.num_indices;
        }
      }
      if (snap.has_video_frame()) {
        video_since_clear = true;
      }
    }
  }
  if (last_resolve_dest_ != 0 && drawn_since_copy) {
    // If the frame cleared the target after resolve and drew ONLY tiny secondary effects
    // (sparks, collision flash, debris <= 8 draws), draw_target cannot restore the scene.
    // In that specific case, revert to the last resolved front buffer.
    const bool only_secondary =
        frame_draws_after_resolve_ <= 8 && frame_indices_after_resolve_ <= 128;
    if (only_secondary && frame_cleared_after_resolve_) {
      drawn_since_copy = false;
      frame_output_dest_ =
          last_front_buffer_resolve_ != 0 ? last_front_buffer_resolve_ : last_resolve_dest_;
    }
  }
  if (drawn_since_copy) {
    frame_output_dest_ = 0;
  }
  frame_drawn_since_copy_ = drawn_since_copy;
  {
    static const bool probing = std::getenv("XERENGE_FRAME_PROBE") != nullptr;
    if (probing) {
      uint32_t clears = 0, copies = 0, d3d = 0, video = 0;
      for (const auto& d : draws) {
        clears += d.is_clear ? 1 : 0;
        copies += d.is_resolve ? 1 : 0;
        d3d += (!d.is_clear && !d.is_resolve && d.d3d_vertex_buffer != 0) ? 1 : 0;
        video += !d.has_video_frame() ? 0 : 1;
      }
      size_t last_copy = draws.size();
      for (size_t i = draws.size(); i-- > 0;) {
        if (draws[i].is_resolve) {
          last_copy = i;
          break;
        }
      }
      std::string tail;
      for (size_t i = last_copy + 1; i < draws.size() && i < last_copy + 8; ++i) {
        const auto& d = draws[i];
        tail += d.is_clear ? " clear"
                           : fmt::format(" draw(vs={:016X} vb={:08X} n={}{})", d.vs_hash,
                                         d.d3d_vertex_buffer, d.num_indices,
                                         !d.has_video_frame() ? "" : " video");
      }
      g_plume_frame_summary = fmt::format(
          "frame {}: {} entries, {} encoded ({} Direct3D, {} video), {} clears, {} copies, shown "
          "from {} {:08X}; after the last copy ({} of {}):{}",
          frame_serial_, draws.size(), encoded, d3d, video, clears, copies,
          drawn_since_copy ? "target" : "copy", frame_output_dest_, last_copy, draws.size(), tail);
    }
  }
  if (pass && pass->mark) {
    pass->mark(pass->context, 0, segment_draws, segment_vertices, segment_binds);
  }
  frame_encoded_draws_ = encoded;
  if (xerenge::Diagnostics()) {
    // A frame with far fewer draws than the ones around it: the black flashes
    // the user sees now and then. Say what it held.
    static double average = 0.0;
    static uint32_t reported = 0;
    if (average > 200.0 && double(encoded) < average * 0.3 && reported < 60) {
      ++reported;
      uint32_t clears = 0, copies = 0, d3d = 0, videos = 0;
      for (const auto& d : draws) {
        clears += d.is_clear ? 1 : 0;
        copies += d.is_resolve ? 1 : 0;
        d3d += (!d.is_clear && !d.is_resolve && d.d3d_vertex_buffer != 0) ? 1 : 0;
        videos += !d.has_video_frame() ? 0 : 1;
      }
      REXLOG_WARN("plume: thin frame {}: encoded {} (average {:.0f}) of {} entries - {} clears, "
                  "{} copies, {} Direct3D draws, {} video; shown from {} {:08X}",
                  frame_serial_, encoded, average, draws.size(), clears, copies, d3d, videos,
                  drawn_since_copy ? "target" : "copy", frame_output_dest_);
    }
    average = average * 0.9 + double(encoded) * 0.1;
    // Frames shown from the target after the ones before were shown from a
    // copy: what was queued after that frame's last copy.
    static bool last_from_copy = false;
    static uint32_t flips = 0;
    const bool from_copy = !drawn_since_copy;
    if (last_from_copy && !from_copy && flips < 40) {
      ++flips;
      size_t last_copy = draws.size();
      for (size_t i = draws.size(); i-- > 0;) {
        if (draws[i].is_resolve) {
          last_copy = i;
          break;
        }
      }
      std::string tail;
      for (size_t i = last_copy + 1; i < draws.size() && i < last_copy + 12; ++i) {
        const auto& d = draws[i];
        tail += d.is_clear ? " clear" : fmt::format(" draw(vs={:016X} vb={:08X} n={})", d.vs_hash,
                                                     d.d3d_vertex_buffer, d.num_indices);
      }
      REXLOG_WARN("plume: frame {} shown from its target after copies: {} entries after the "
                  "last copy (at {} of {}):{}",
                  frame_serial_, draws.size() - (last_copy == draws.size() ? 0 : last_copy + 1),
                  last_copy, draws.size(), tail);
    }
    last_from_copy = from_copy;
  }

  if (!packed_writes_.empty()) {
    CheckPackedWrites();
  }
  EncodeStage("idle");
  // Eight lines only ever described the opening seconds, and this is the one
  // place that says whether a draw that survived every filter actually reached
  // the command list. Keep reporting, at a rate that does not drown the log.
  static uint32_t encode_logs = 0;
  ++encode_logs;
  if (encode_logs <= 8 || (encode_logs % 120) == 0) {
    const GuestDrawSnapshot& last = draws.back();
    REXLOG_INFO(
        "plume: encoded {}/{} DRAWs ({} guest) last vs={:016X} ps={:016X} prim={} indices={}",
        encoded, draws.size(), guest_draws, last.vs_hash, last.ps_hash, last.prim_type,
        last.num_indices);
  }
}

}  // namespace rex::plume_renderer
