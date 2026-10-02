/**
 * @file        plume_renderer/plume_draw.h
 * @brief       Guest DRAW_INDX_2 → plume graphics pipeline (present thread).
 */
#pragma once

#include <unordered_set>

#include <array>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_types.h>
#include <plume_render_interface_builders.h>

namespace rex::memory {
class Memory;
}

namespace rex::graphics {
struct TextureInfo;
}

namespace rex::plume_renderer {

// A draw's constant words, shared between copies of its snapshot and copied
// only when one of them is written. A snapshot carries 9 KB of these, and a
// thousand snapshots a frame were copied three and four times over - into the
// ring, out of it, into the frame's batch and into the last frame's - which
// was most of what the present thread did on a laptop. The interface is
// std::array's, so what reads them is unchanged; reading through a const
// snapshot never copies.
template <size_t N>
class SharedWords {
 public:
  using Words = std::array<uint32_t, N>;

  SharedWords() : words_(Zeroes()) {}
  SharedWords(const Words& words) : words_(std::make_shared<Words>(words)) {}
  SharedWords& operator=(const Words& words) {
    words_ = std::make_shared<Words>(words);
    return *this;
  }
  operator Words() const { return *words_; }

  // The words at src - as last's block when they are the same, which they
  // mostly are from one draw to the next; last then follows the new block.
  // A fresh 9 KB copy per draw was a quarter of the title's thread on the
  // Pixel, the allocator returning the memory included.
  void AssignShared(const uint32_t* src, SharedWords& last) {
    if (last.words_ != Zeroes() && std::memcmp(last.words_->data(), src, sizeof(Words)) == 0) {
      words_ = last.words_;
      return;
    }
    auto fresh = std::make_shared<Words>();
    std::memcpy(fresh->data(), src, sizeof(Words));
    words_ = fresh;
    last.words_ = std::move(fresh);
  }

  static constexpr size_t size() { return N; }
  const uint32_t* data() const { return words_->data(); }
  uint32_t* data() {
    Detach();
    return words_->data();
  }
  const uint32_t& operator[](size_t i) const { return (*words_)[i]; }
  uint32_t& operator[](size_t i) {
    Detach();
    return (*words_)[i];
  }
  void fill(uint32_t value) {
    if (value == 0) {
      words_ = Zeroes();
      return;
    }
    Detach();
    words_->fill(value);
  }

 private:
  static const std::shared_ptr<Words>& Zeroes() {
    static const std::shared_ptr<Words> zeroes = std::make_shared<Words>();
    return zeroes;
  }
  // Every snapshot starts on the shared zeroes, so the first write always
  // copies, and a fresh one is written by the thread that owns it alone.
  void Detach() {
    if (words_ == Zeroes() || words_.use_count() != 1) {
      words_ = std::make_shared<Words>(*words_);
    }
  }

  std::shared_ptr<Words> words_;
};

struct VfetchAttr {
  uint32_t fetch_const = 0;
  // Position of this vertex fetch instruction in the shader program. This is
  // what a container's vertex element declaration is keyed by - XenosRecomp
  // matches a declaration to the instruction at that address, not to the
  // fetch constant the instruction reads.
  uint32_t instr_address = 0;
  uint32_t stride_dwords = 0;
  int32_t offset_dwords = 0;
  uint32_t format = 0;
  int32_t exp_adjust = 0;
  uint32_t location = 0;
  bool is_signed = false;
  bool is_normalized = true;
  // Where the fetch takes its index from and where its result goes - needed
  // for shaders that fetch one stream by an index read out of another.
  uint32_t src_reg = 0;
  uint32_t src_comp = 0;
  uint32_t dst_reg = 0;
  uint32_t dst_swiz = 0;
  bool index_rounded = false;
};

struct ResolvedShaderVfetch {
  std::vector<VfetchAttr> passthrough_attrs;
  std::vector<VfetchAttr> real_attrs;
  int32_t passthrough_pos_fetch_const = -1;
  uint32_t passthrough_pos_float = 0;
  int32_t real_pos_fetch_const = -1;
  uint32_t real_pos_float = 0;
  bool is_index_instanced = false;
  bool is_vertex_fetch = false;
  bool is_position_scaling = false;
};

// Lets whoever encodes a frame close the render pass and open it again. The
// title copies its render target mid-frame and then draws over the top from
// what it copied, and copying out of a target still bound for writing is
// invalid - so the copy has to sit between two passes, at the point in the
// draw order where it was asked for.
struct RenderPassBreak {
  void* context = nullptr;
  void (*end)(void*) = nullptr;
  void (*begin)(void*) = nullptr;
  // The second colour attachment, when the pass has one: the title's render
  // target 1, which a resolve with source 1 copies from.
  plume::RenderTexture* second_target = nullptr;
  // Debug mode: a GPU timestamp between two passes, after the copy into `dest`
  // that ended the first one, which held `draws` draws. Dest 0: the frame's
  // last pass, which the swapchain times itself. The swapchain reports what
  // the GPU spent between marks (PlumeSwapchain::MarkPass).
  void (*mark)(void*, uint32_t dest, uint32_t draws) = nullptr;
};

// Whether scene passes carry a second colour attachment - render target 1,
// which Burnout's scene shaders write their motion vectors into. Every
// pipeline drawn in such a pass has to declare both.
bool PlumeSecondTargetEnabled();

// What the last encoded frame held, in one line (for the frame probe).
extern std::string g_plume_frame_summary;

struct GuestDrawSnapshot {
  uint64_t vs_hash = 0;
  uint64_t ps_hash = 0;
  uint32_t prim_type = 0;
  uint32_t source_select = 0;
  uint32_t num_indices = 0;
  uint32_t vte_cntl = 0;
  // RB_BLENDCONTROL0 / RB_COLOR_MASK as the guest set them for this draw.
  // Burnout's UI draws the same text more than once with different blend
  // modes (a coverage/underlay pass and a coloured fill pass); forcing one
  // fixed alpha blend on every draw turned the underlay into a white halo
  // and made translucent panels render opaque.
  uint32_t blend_control = 0;
  uint32_t color_mask = 0xFu;
  // RB_DEPTHCONTROL. Screen-space UI draws it disabled; 3D geometry relies on
  // it, and without honouring it the scene draws in submission order.
  uint32_t depth_control = 0;
  float vport_xscale = 0.0f;
  float vport_xoffset = 0.0f;
  float vport_yscale = 0.0f;
  float vport_yoffset = 0.0f;
  float vport_zscale = 0.0f;
  float vport_zoffset = 0.0f;
  SharedWords<1024> vs_constants;
  SharedWords<1024> ps_constants;
  SharedWords<192> fetch_constants;
  // Set when the draw came from the title's Direct3D call rather than from a
  // packet: then the buffers are named outright and there is nothing to infer
  // from the fetch constants, which at that moment describe a different draw.
  uint32_t d3d_vertex_buffer = 0;
  uint32_t d3d_vertex_stride = 0;
  uint32_t d3d_index_buffer = 0;
  uint32_t d3d_base_vertex = 0;
  uint32_t d3d_start_index = 0;
  bool d3d_index_32bit = false;
  // The title's vertex streams (SetStreamSource), stream i = fetch constant
  // 95 - i: data address and stride in bytes.
  uint32_t d3d_stream_address[4] = {};
  uint32_t d3d_stream_stride[4] = {};
  // The render target the Direct3D draw went into, in its own pixels.
  uint32_t d3d_target_width = 0;
  // A second render target bound alongside the first (Direct3D draws).
  bool rt1_bound = false;
  // Drawn by the title's 2D layer (GuestDrawBuffers::interface): kept in the
  // 16:9 box on a wider screen.
  bool interface_draw = false;
  // ... with that box against the screen's left edge (the music player).
  bool interface_left = false;
  // The title's 2D object the draw belongs to (0: none), moved to an edge whole.
  uint32_t interface_object = 0;
  // Alpha test (RB_COLORCONTROL, device + 0x2D7C) and its reference (device +
  // 0x2D44): the translated shaders discard below the threshold when the
  // pipeline's specialisation constant asks them to.
  bool alpha_test = false;
  float alpha_ref = 0.0f;
  // Polygon offset for front faces (PA_SU_SC_MODE_CNTL bit 11, device + 0x2D88):
  // PA_SU_POLY_OFFSET_FRONT_SCALE at + 0x2E90 (1/16 subpixel units) and
  // _FRONT_OFFSET at + 0x2E94 (depth range units), as SetRenderState_DepthBias
  // and _SlopeScaleDepthBias store them. Decals on the road need it.
  // PA_SU_SC_MODE_CNTL bits 0-2: cull front, cull back, and which winding is
  // the front (set: clockwise). Honoured under XERENGE_CULL (see
  // GetOrCreatePipeline).
  uint32_t cull = 0;
  bool poly_offset = false;
  float poly_offset_scale = 0.0f;
  float poly_offset_offset = 0.0f;
  // The title's scissor rectangle (D3DDevice_SetScissorRect, device + 0x3220)
  // in render target pixels, when its scissor test is on (device + 0x2EB0).
  bool scissor_enabled = false;
  int32_t scissor_rect[4] = {};
  uint32_t d3d_target_height = 0;
  // Not a draw at all: a marker for where the title asked the render target to
  // be copied out. It has to keep its place among the draws, because the title
  // copies mid-frame and then draws over the top using what it copied.
  bool is_resolve = false;
  bool is_end_tiling = false;
  // Where the title called Swap (see NoteGuestFrameEnd); never drawn.
  bool is_frame_end = false;
  // Where the video blit happened among the draws, with targets followed:
  // the decoded frame is put here at present rather than in front of the
  // frame, because the title copies it out of its target right after.
  bool video_slot = false;
  // Likewise a marker, for where the title cleared its render target. Only
  // depth is acted on: the colour target is shared with the interface.
  bool is_clear = false;
  uint32_t clear_flags = 0;
  float clear_depth = 1.0f;
  float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool clear_whole = false;
  // A smaller target's whole-target clear: its size, cleared in the corner it is
  // drawn in (0 for the frame's own target).
  uint32_t clear_width = 0;
  uint32_t clear_height = 0;
  uint32_t resolve_dest = 0;
  // Which target the copy reads: Direct3D's low three flag bits - 0 and 1 are
  // colour targets 0 and 1, 4 is depth.
  uint32_t resolve_source = 0;
  uint32_t resolve_flags = 0;
  // Into which face of a cube map, when the destination is one.
  uint32_t resolve_face = 0;
  bool resolve_cube = false;
  uint32_t resolve_width = 0;
  uint32_t resolve_height = 0;
  uint32_t vs_bool = 0;
  uint32_t ps_bool = 0;
  bool valid = false;
  // Copied at DRAW time. Present-time reads race the decoder and strobe gray.
  std::vector<uint8_t> video_rgba;
  uint32_t video_width = 0;
  uint32_t video_height = 0;
  uint32_t video_row_bytes = 0;
  uint32_t video_row_texels = 0;
  uint64_t video_key = 0;
  uint32_t video_luma_mean = 0;
  uint32_t video_luma_range = 0;
  // XERENGE_VIDEO_GPU 2 and 3: the frame's planes as decoded, for the video
  // pipeline to turn into RGB on the GPU (video.frag). Luma, then chroma - one
  // plane of Cb,Cr pairs when packed, else Cb and Cr.
  std::vector<uint8_t> video_y;
  std::vector<uint8_t> video_u;
  std::vector<uint8_t> video_v;
  bool video_packed_uv = false;
  uint32_t video_y_row_texels = 0;
  uint32_t video_u_width = 0;
  uint32_t video_u_height = 0;
  uint32_t video_u_row_texels = 0;
  uint32_t video_v_width = 0;
  uint32_t video_v_height = 0;
  uint32_t video_v_row_texels = 0;
  uint64_t video_u_key = 0;
  uint64_t video_v_key = 0;
  // The planes' guest memory as it was when the title drew the frame, slot
  // by slot (CopyGuestVideoPlanes): the decoder writes the next frame into
  // the same memory, and reading it later, on the capture worker, tore it.
  std::vector<uint8_t> video_source[3];

  // A decoded frame, in either form.
  bool has_video_frame() const { return !video_rgba.empty() || !video_y.empty(); }
  void clear_video_frame() {
    video_rgba.clear();
    video_y.clear();
    video_u.clear();
    video_v.clear();
  }
};

// Copies a video draw's planes out of guest memory, as they are, for
// CaptureGuestVideoFrame to work from later. Cheap - a straight copy - so it
// runs on the thread that saw the draw, while the frame is still there.
void CopyGuestVideoPlanes(GuestDrawSnapshot* snap, memory::Memory* memory);
bool CaptureGuestVideoFrame(GuestDrawSnapshot* snap, memory::Memory* memory);
bool IsGuestVideoBlit(const GuestDrawSnapshot& snap, uint64_t* key_out = nullptr);

// One mip level below a texture's base, decoded to the host format like the
// base level.
struct HostMipLevel {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_texels = 0;
};

// What used to be a bindless descriptor set: the textures (or samplers) the
// draws refer to by index, kept on the processor. Indices stay what the rest
// of the renderer hands out; each draw gets a small descriptor set of its own
// holding just its slots (PlumeDrawContext::DrawSetFor), which needs no
// descriptor indexing - the stock Vulkan 1.1 drivers of most phones lack it.
// Every change of an entry bumps its version, so a cached set that held the
// old one is never bound again.
class BindlessTable {
 public:
  struct Entry {
    const plume::RenderTexture* texture = nullptr;
    const plume::RenderTextureView* view = nullptr;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::SHADER_READ;
    const plume::RenderSampler* sampler = nullptr;
    uint32_t version = 0;
  };
  explicit BindlessTable(uint32_t count) : entries_(count) {}
  void setTexture(uint32_t index, const plume::RenderTexture* texture, plume::RenderTextureLayout layout,
                  const plume::RenderTextureView* view = nullptr) {
    if (index < entries_.size()) {
      Entry& e = entries_[index];
      // The same texture registered again (a video plane or a copy refreshed in
      // place each frame) keeps its version: the sets holding it stay good.
      if (e.texture == texture && e.view == view && e.layout == layout && e.version != 0) {
        return;
      }
      e.texture = texture;
      e.layout = layout;
      e.view = view;
      e.version = ++next_version_;
    }
  }
  void setSampler(uint32_t index, const plume::RenderSampler* sampler) {
    if (index < entries_.size()) {
      if (entries_[index].sampler == sampler && entries_[index].version != 0) {
        return;
      }
      entries_[index].sampler = sampler;
      entries_[index].version = ++next_version_;
    }
  }
  const Entry& at(uint32_t index) const { return entries_[index < entries_.size() ? index : 0]; }
  // Grows with every change of an entry: equal means nothing changed since.
  uint32_t changes() const { return next_version_; }

 private:
  std::vector<Entry> entries_;
  uint32_t next_version_ = 0;
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
  // Logs the next encoded frame's queue, tagged - so a RenderDoc capture taken
  // by hand comes with the list of draws the frame was made of.
  void RequestQueueDump(std::string tag) {
    std::lock_guard lock(dump_request_mutex_);
    dump_request_tag_ = std::move(tag);
    dump_requested_ = true;
  }

  void RegisterVsUcode(uint64_t shader_hash, const uint8_t* bytes, uint32_t byte_size);
  // Renders the vertex-fetch declarations XenosRecomp's container wrapper
  // wants (--element ADDRESS:USAGE:INDEX), derived from the same microcode
  // parse the renderer already relies on. A shader the title assembles at
  // runtime has no container in the XEX, so the static scan never sees it,
  // and these have to be reconstructed before it can be translated at all.
  static std::string DescribeVsWrapperElements(const uint8_t* bytes, uint32_t byte_size);
  // The same for a pixel shader: which interpolators it consumes, named with
  // the convention the vertex side above emits, so a wrapped pair links up.
  static std::string DescribePsWrapperInterpolators(const uint8_t* bytes, uint32_t byte_size);
  // The colour target is needed here as well as by the caller: a copy the
  // title asked for mid-frame has to happen between the draws, not after them.
  void EncodeDraws(plume::RenderCommandList* list, const std::vector<GuestDrawSnapshot>& draws,
                   memory::Memory* memory, uint32_t width, uint32_t height,
                   plume::RenderTexture* colour_target = nullptr,
                   const RenderPassBreak* pass = nullptr);

  // Copies what was just rendered into a texture standing in for the guest
  // memory at dest_base. Later draws that fetch from that address get this
  // image instead of whatever untiling its (never written) memory would
  // produce - which is how a title gets to reuse a rendered scene as a
  // texture.
  // With render targets followed, what the console shows is the front buffer
  // the frame was last resolved into, not the render target - the title clears
  // that for the next frame straight after resolving. Copies that picture over
  // `color` once the pass is closed; does nothing if there was no such copy.
  // Whether to show the last picture again instead of this frame: it holds no
  // Direct3D draw and no copy - a clear, perhaps a menu video frame - while the
  // frames around it draw dozens or more (the menus over their background video on a
  // slow device: every other present was the bare video, and the menu blinked).
  // Pure video, with no interface drawn at all, brings the average down within
  // half a second and is shown again. Call once per frame.
  bool HoldsThinFrame(const std::vector<GuestDrawSnapshot>& draws);

  void PresentResolvedFrame(plume::RenderCommandList* list, plume::RenderTexture* color,
                            uint32_t width, uint32_t height, uint32_t front_buffer);
  void ResolveRenderTarget(plume::RenderCommandList* list, plume::RenderTexture* color,
                           uint32_t width, uint32_t height, uint32_t dest_base,
                           uint32_t dest_width, uint32_t dest_height, uint32_t region_width = 0,
                           uint32_t region_height = 0, uint32_t face = 0, bool cube = false);

 private:
  struct PipelineKey {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t topology = 0;
    uint32_t state = 0;
    // The alpha half of RB_BLENDCONTROL, which `state` has no room for: two
    // draws differing only there must not share a pipeline.
    uint32_t alpha_blend = 0;

    bool operator==(const PipelineKey& other) const {
      return vs == other.vs && ps == other.ps && topology == other.topology &&
             state == other.state && alpha_blend == other.alpha_blend;
    }
  };
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const {
      return size_t(key.vs ^ (key.ps * 0x9E3779B97F4A7C15ull) ^ uint64_t(key.topology) ^
                    (uint64_t(key.state) << 8) ^ (uint64_t(key.alpha_blend) << 40));
    }
  };

  plume::RenderPrimitiveTopology MapTopology(uint32_t prim_type) const;
  // Pipeline built from the title's own translated shaders, as opposed to the
  // passthrough pair. Guest blend and depth state take part in the key, since
  // they are baked into the pipeline.
  plume::RenderPipeline* GetOrCreatePipeline(uint64_t vs_hash, uint64_t ps_hash,
                                             plume::RenderPrimitiveTopology topology,
                                             uint32_t blend_control, uint32_t color_mask,
                                             uint32_t depth_control, bool alpha_test = false,
                                             bool depth_bias = false,
                                             uint32_t cull = 0);
  uint32_t FillVertices(const GuestDrawSnapshot& snap, memory::Memory* memory,
                        uint32_t base_vertex, bool passthrough);
  void UploadNullTexture(plume::RenderCommandList* list);
  bool UploadHostTexture(plume::RenderCommandList* list, uint64_t key, uint32_t width,
                         uint32_t height, plume::RenderFormat host_format,
                         const std::vector<uint8_t>& pixels, uint32_t row_texels,
                         const std::vector<HostMipLevel>* mips = nullptr);
  void BindGuestTextures(plume::RenderCommandList* list, const std::vector<GuestDrawSnapshot>& draws,
                         memory::Memory* memory);
  uint32_t BindlessForTexture(const GuestDrawSnapshot& snap) const;
  // this_frame: only a copy made earlier in the frame being encoded - an
  // address the title copied into once, in a menu, and later reuses for an
  // ordinary texture does not count.
  bool ReadsResolvedCopy(const GuestDrawSnapshot& snap, bool this_frame = false) const;
  uint32_t BindlessForSlot(const GuestDrawSnapshot& snap, uint32_t slot) const;
  void FillSharedConstants(uint8_t* dst, const GuestDrawSnapshot& snap, uint32_t width,
                           uint32_t height) const;
  // Builds the pipelines a scene draw would need, with no title involved.
  void Run3DCapabilityCheck();
  bool CreatePassthroughPipeline();
  // Returns the passthrough pipeline matching this topology and the guest's
  // own blend state, creating it on first use.
  plume::RenderPipeline* PassthroughFor(plume::RenderPrimitiveTopology topology,
                                        uint32_t blend_control, uint32_t color_mask,
                                        uint32_t depth_control);
  // The same for the video frame's own pipeline (video.vert/video.frag).
  plume::RenderPipeline* VideoPipelineFor(plume::RenderPrimitiveTopology topology,
                                          uint32_t blend_control, uint32_t color_mask,
                                          uint32_t depth_control);
  // The slot of a video plane uploaded for the frame, 0 if it is not there.
  uint32_t VideoPlaneSlot(uint64_t key) const;
  // Whether this draw is a video frame the video pipeline draws: stage 3,
  // and all of its planes uploaded.
  bool UsesVideoPipeline(const GuestDrawSnapshot& snap) const;
  // Both of the above: a screen-space pipeline from a shader pair, cached.
  plume::RenderPipeline* ScreenPipelineFor(
      plume::RenderShader* vs, plume::RenderShader* ps,
      std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>>& cache,
      const char* what, plume::RenderPrimitiveTopology topology, uint32_t blend_control,
      uint32_t color_mask, uint32_t depth_control);

  struct GuestHostTexture {
    uint64_t key = 0;
    uint32_t bindless = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    plume::RenderFormat format = plume::RenderFormat::R8G8B8A8_UNORM;
    // Mip levels, the base included.
    uint32_t levels = 1;
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    // Two, used alternately by frame: the GPU may still be copying from the
    // one the last frame filled while this frame fills the other.
    std::unique_ptr<plume::RenderBuffer> staging[2];
    bool uploaded = false;
    // Hash of the guest memory this was built from, and the size of the
    // staging buffer already allocated for it. Guest textures are mostly
    // static - re-untiling and re-uploading one whose bytes have not moved is
    // pure cost, and so is allocating a staging buffer that already exists at
    // the right size.
    uint64_t content_hash = 0;
    size_t staging_size[2] = {0, 0};
    // The frame this texture was last bound in, for evicting the stalest one
    // when the bindless slots run out.
    uint64_t last_used = 0;
  };

  // Textures holding resolved render targets, keyed by the guest address the
  // guest resolved them to. A fetch from one of these addresses is served from
  // here rather than by untiling guest memory, which for a resolve target
  // holds nothing this renderer ever wrote.
  struct ResolvedTarget {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t bindless = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // A cube map: six faces, each filled by its own copy, and bound through
    // the cube descriptor set rather than the 2D one.
    bool cube = false;
  };
  std::unordered_map<uint32_t, ResolvedTarget> resolved_targets_;

  // Input locations a translated shader actually uses, per shader hash. These
  // are NOT interchangeable with the ones in vfetch_by_shader_: those follow
  // this renderer's own convention, which is what the passthrough shader
  // expects, while these come from the shader's own declarations. Applying the
  // translated numbering to a passthrough draw scatters its vertices.
  std::unordered_map<uint64_t, std::vector<uint32_t>> real_locations_by_shader_;

  // Texture keys already handled during the current BindGuestTextures call.
  // The same atlas is bound by many draws in a frame, and without this each
  // of them repeats the whole untile-convert-upload for identical bytes.
  std::vector<uint64_t> textures_done_this_frame_;

  plume::RenderDevice* device_ = nullptr;
  bool ready_ = false;
  bool null_texture_uploaded_ = false;
  // Whether the GPU takes BC (DXT) textures; without, they become ETC2 where
  // it takes that (TranscodeBcToEtc2), RGBA8 otherwise.
  bool bc_supported_ = true;
  bool etc2_supported_ = false;
  // XERENGE_FRAME_PROBE: the middle of what a resolve copies from, read back a few
  // frames later - whether the draws themselves came out black (ResolveRenderTarget).
  std::unique_ptr<plume::RenderBuffer> resolve_probe_;
  void* resolve_probe_mapped_ = nullptr;
  uint64_t resolve_probe_frame_ = 0;
  bool resolve_probe_pending_ = false;
  bool last_fill_passthrough_ = false;
  // The last filled draw's horizontal extent in its target's pixels, when its
  // positions were pixels (an interface draw): which edge its object goes to.
  bool last_fill_x_known_ = false;
  float last_fill_x_[2] = {0.0f, 0.0f};

  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout_;
  std::unique_ptr<BindlessTable> texture_set_;
  std::unique_ptr<BindlessTable> sampler_set_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  std::unique_ptr<plume::RenderTexture> null_texture_;
  std::unique_ptr<plume::RenderTextureView> null_texture_view_;
  std::unique_ptr<plume::RenderBuffer> null_texture_staging_;
  // Descriptor set 1: volume textures, which the translated shaders sample as
  // two-dimensional arrays. Until the title's own are uploaded, every slot
  // holds an identity colour table - the 3D frame's final pass grades its
  // colour through one, and with nothing there it came out black.
  std::unique_ptr<BindlessTable> volume_set_;
  // Samplers built from the guest's fetch constants: address modes and
  // filters. Every texture was sampled with one nearest/clamp sampler, so a
  // texture meant to repeat - the road, the wheel rims - showed its edge
  // stretched across everything past 1.0.
  std::unordered_set<uint64_t> index_instanced_shaders_;
  // Vertex shaders that scale their position themselves (the output
  // position is computed with an arithmetic operator rather than handed on).
  // The menu's video is drawn with one: pixels * (2/1280, -2/720) + (-1, 1).
  // Converting its pixels to clip space first shrank the quad into the
  // top-left corner on every frame whose copy it read was made that frame.
  std::unordered_set<uint64_t> position_scaling_shaders_;
  // Vertex shaders that sample textures (the sky's gradient).
  std::unordered_set<uint64_t> vertex_fetch_shaders_;
  // Where the title copies its video frame to (resolves made right after the
  // video was drawn). A later copy there with no video drawn since the last
  // clear would copy a cleared target, so it is skipped and the last frame kept.
  // Only while the video is playing: the frame serial of the last copy with video
  // into each. A loading screen with no video at all must copy normally - kept
  // forever, every other frame there showed an old copy and the bar jumped back.
  std::unordered_map<uint32_t, uint64_t> video_copy_dests_;
  // Whether a pixel shader writes its second colour output (oC1), from its
  // generated source. One that does not must not have render target 1
  // enabled: Vulkan leaves an unwritten output undefined, and undefined
  // motion vectors turned into noise across the whole frame.
  std::unordered_map<uint64_t, bool> ps_writes_oc1_;
  bool PixelShaderWritesOc1(uint64_t ps_hash);
  // Which shared-constant texture slots a shader's generated source declares
  // (bit per slot), read once per shader.
  uint32_t ShaderSamplerSlots(uint64_t hash) const;
  // Fetch slots (bit per slot, 0-31) a draw's shaders can sample; ~0u when
  // that is not known and every slot has to be considered.
  uint32_t UsedTextureSlots(const GuestDrawSnapshot& snap) const;
  mutable std::unordered_map<uint64_t, uint32_t> sampler_slots_by_shader_;
  uint64_t frame_serial_ = 0;
  std::mutex dump_request_mutex_;
  bool dump_requested_ = false;
  std::string dump_request_tag_;
  // A bindless slot for a new guest texture: a fresh one while any are left,
  // then the slot of the texture unused for longest (and not in the last few
  // frames, which may still be on the GPU). 0 when none can be had.
  uint32_t AcquireTextureSlot();
  std::unordered_map<uint32_t, uint32_t> sampler_index_by_key_;
  std::vector<std::unique_ptr<plume::RenderSampler>> guest_samplers_;
  uint32_t next_sampler_ = 1;
  uint32_t SamplerKey(const GuestDrawSnapshot& snap, uint32_t slot) const;
  void EnsureSampler(const GuestDrawSnapshot& snap, uint32_t slot);
  // Guest textures with more than one layer - stacked 2D or volume - as 2D
  // arrays in volume_set_. The 3D frame's colour grading table is one: 32
  // slices of 32x32. Index 0 of the set stays the identity table.
  struct LayeredTexture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderBuffer> staging[2];
    size_t staging_size[2] = {0, 0};
    uint32_t index = 0;
    uint64_t content = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
  };
  std::unordered_map<uint64_t, LayeredTexture> layered_textures_;
  uint32_t next_layered_index_ = 1;
  void UploadLayeredTexture(plume::RenderCommandList* list, uint64_t key,
                            const rex::graphics::TextureInfo& info, const uint8_t* src,
                            plume::RenderFormat host_format, bool expand_r8, bool swap_bgra);
  // Descriptor set 2: cube maps. Every slot holds a black cube until a copy
  // fills one - sampling the 2D set's views as cubes read garbage, which is
  // what the car's reflections were.
  std::unique_ptr<BindlessTable> cube_set_;
  std::unique_ptr<plume::RenderTexture> null_cube_;
  std::unique_ptr<plume::RenderTextureView> null_cube_view_;
  std::unique_ptr<plume::RenderTexture> identity_lut_;
  std::unique_ptr<plume::RenderTextureView> identity_lut_view_;
  std::unique_ptr<plume::RenderBuffer> identity_lut_staging_;
  std::unique_ptr<plume::RenderShader> passthrough_vs_;
  std::unique_ptr<plume::RenderShader> passthrough_ps_;
  // Keyed by topology + the guest blend state the draw asked for, built on
  // first use. The guest switches blend mode between UI passes, so a single
  // pipeline per topology cannot serve them all.
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> passthrough_pipelines_;
  // The video frame's own pipelines (XERENGE_VIDEO_GPU): YUV planes to RGB.
  std::unique_ptr<plume::RenderShader> video_vs_;
  std::unique_ptr<plume::RenderShader> video_ps_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> video_pipelines_;

  // Shader coverage. A draw can only leave the passthrough shader when both
  // of its guest shaders exist in the translated cache, so this predicate is
  // what the real-shader path has to consult per draw anyway. Recording it
  // first answers the question that decides whether that path is worth
  // building for this title at all: geometry drawn with a missing shader
  // renders nothing, so depth buffers and render targets are premature until
  // coverage is known. Keyed by guest shader hash -> present in the cache.
  std::unordered_map<uint64_t, bool> vs_coverage_;
  std::unordered_map<uint64_t, bool> ps_coverage_;
  uint64_t draws_seen_ = 0;
  uint64_t draws_both_shaders_ = 0;
  void RecordShaderCoverage(uint64_t vs_hash, uint64_t ps_hash);

  // VS constant slot each UI shader writes oD0 from, learned from the draws
  // where the colour is unambiguous (white/gold fills). Once known it is read
  // directly, so a pass whose colour is genuinely black - a text drop shadow -
  // is no longer rejected as "not colour-like" and painted white instead.
  std::unordered_map<uint64_t, int> ui_tint_slot_by_shader_;

  std::unique_ptr<plume::RenderBuffer> vs_constants_;
  std::unique_ptr<plume::RenderBuffer> ps_constants_;
  std::unique_ptr<plume::RenderBuffer> shared_constants_;
  std::unique_ptr<plume::RenderBuffer> dummy_vb_;
  // Unpacked vertices of the scene's indexed draws, kept between frames. The
  // garage re-unpacked the same half million vertices every frame - two
  // thirds of the frame - so a draw whose inputs hash the same as last time
  // is drawn straight from here. Reset whole, at the start of a frame, when
  // nearly full: frames are synchronous, so nothing is still reading it.
  std::unique_ptr<plume::RenderBuffer> cache_vb_;
  float* cache_mapped_ = nullptr;
  plume::RenderVertexBufferView cache_view_{};
  uint32_t cache_used_ = 0;
  struct CachedMesh {
    uint32_t offset = 0;
    uint32_t count = 0;
    uint64_t index_hash = 0;
    uint64_t data_hash = 0;
    uint32_t changes = 0;
    uint64_t last_used = 0;  // frame_serial_ of the last frame that drew it
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // physical byte [start, end)
    // The first packed vec4 of the first and the last vertex as stored, read
    // back on a hit with XERENGE_VERTEX_TRACE to catch a place someone else
    // wrote over (see CacheHoldsMesh).
    std::array<float, 8> fingerprint{};
    uint32_t packed_floats = 0;  // floats per stored vertex
  };
  // Whether the cache buffer at a mesh's place still holds what was stored there.
  bool CacheHoldsMesh(uint64_t key, const CachedMesh& entry) const;
  std::unordered_map<uint64_t, CachedMesh> mesh_cache_;
  // The cache buffer is a ring: new meshes go at cache_used_ and push out the
  // ones stored there longest ago, never one drawn in the frame being
  // encoded (the GPU reads it when the frame is submitted). Emptying the whole
  // cache when it filled - every half minute or so in a race, as the track
  // streams in - cost a frame long enough to see.
  std::map<uint32_t, uint64_t> cache_by_offset_;  // offset -> key
  // The places of meshes stored again elsewhere (their vertices changed): still
  // read by the frame that drew them, and by the one before it on the GPU, so
  // nothing may be stored over them until those frames are done.
  struct RetiredRegion {
    uint32_t offset = 0;
    uint32_t count = 0;
    uint64_t last_used = 0;
  };
  std::vector<RetiredRegion> retired_cache_regions_;
  bool AllocateCacheRegion(uint32_t count, uint32_t* offset);
  // Keys seen once and not cached yet (a draw is cached from its second
  // sighting). Kept apart so that the one-off draws piling up here - a buffer
  // refilled every frame gives a new key every frame - are dropped on their
  // own, not together with everything cached: that emptied the whole cache
  // every few seconds in the race.
  std::unordered_set<uint64_t> mesh_seen_once_;
  // Guest writes to the pages textures come from, counted per 4 KB page by the
  // memory system's write watch. A texture whose pages have not been written
  // since it was last hashed keeps its hash without its bytes being read
  // again - hashing every texture of the frame was 40 MB a frame in the race.
  std::unique_ptr<std::atomic<uint32_t>[]> page_writes_;
  memory::Memory* watched_memory_ = nullptr;
  void* write_watch_handle_ = nullptr;
  struct TextureWatch {
    uint64_t hash = 0;
    uint64_t writes = 0;  // sum of its pages' write counts when hashed
  };
  std::unordered_map<uint64_t, TextureWatch> texture_watch_;
  static std::pair<uint32_t, uint32_t> OnGuestWrite(void* context, uint32_t start,
                                                    uint32_t length, bool exact_range);
  uint64_t PageWrites(uint32_t start, size_t size) const;
  // A draw's vertex cache key, and whether its cached vertices are still good.
  // Read-only, so it can run for every draw of a frame at once on the worker
  // threads before the draws are encoded one by one.
  struct MeshCheck {
    bool eligible = false;  // could be cached at all
    uint64_t key = 0;
    uint64_t index_hash = 0;
    bool hit = false;
    uint32_t offset = 0;
    uint32_t count = 0;
    bool unstable = false;  // changes too often to be worth caching
  };
  void UnpackVertices(const GuestDrawSnapshot& snap, const std::vector<VfetchAttr>& attrs,
                      int32_t pos_fetch_const, uint32_t pos_float, uint32_t vertex_count,
                      memory::Memory* memory, float* staged, std::vector<uint32_t>& read_lo, std::vector<uint32_t>& read_hi,
                      uint32_t& fetched, uint32_t& fetch_addr, uint32_t& fetch_type) const;
  // Draws of this frame unpacked before encoding, into pre_arena_ (kept
  // between frames so it is not reallocated every frame).
  struct PreUnpacked {
    size_t offset = 0;  // in floats
    uint32_t count = 0;
    std::vector<uint32_t> read_lo, read_hi;
    uint32_t fetched = 0, fetch_addr = 0, fetch_type = 0;
  };
  std::unordered_map<const GuestDrawSnapshot*, PreUnpacked> pre_unpacked_;
  std::vector<float> pre_arena_;
  MeshCheck CheckMeshCache(const GuestDrawSnapshot& snap, const std::vector<VfetchAttr>& attrs,
                           uint32_t vertex_count, memory::Memory* memory) const;
  // This frame's checks, made before encoding, by draw.
  std::unordered_map<const GuestDrawSnapshot*, MeshCheck> mesh_checks_;
  // Set by FillVertices: where in cache_vb_ the draw's vertices are, or ~0u
  // when they went into the per-frame buffer as usual.
  uint32_t last_fill_cache_offset_ = ~0u;
  // The end of this frame's half of the vertex buffer (see EncodeDraws).
  uint32_t vb_limit_ = 0;
  uint64_t cache_hits_ = 0;
  uint64_t cache_misses_ = 0;
  uint64_t cache_resets_ = 0;
  // Vertices are assembled here, in ordinary cached memory, and copied into
  // the upload heap once per draw.
  std::vector<float> stage_;
  // Destination of the last resolve made in draw order this frame.
  uint32_t frame_output_dest_ = 0;
  // Draws encoded this frame, and the copy shown last - a frame that drew
  // nothing and copied nothing shows that copy again (see PresentResolvedFrame).
  uint32_t frame_encoded_draws_ = 0;
  double hold_average_ = 0.0;  // Direct3D draws per frame, for HoldsThinFrame
  bool last_shown_from_copy_ = false;  // the last frame was shown from a copy (PresentResolvedFrame)
  uint32_t last_output_dest_ = 0;
  std::unordered_set<uint32_t> frame_resolved_dests_;
  uint32_t last_resolve_dest_ = 0;
  uint32_t last_front_buffer_resolve_ = 0;
  bool frame_cleared_after_resolve_ = false;
  bool frame_cleared_whole_ = false;
  bool frame_drawn_since_copy_ = true;
  uint32_t frame_draws_after_resolve_ = 0;
  uint32_t frame_indices_after_resolve_ = 0;
  std::unordered_set<uint32_t> known_front_buffers_{0x06C90000, 0x068F8000};

  bool IsKnownFrontBuffer(uint32_t addr) const {
    return addr == 0x06C90000 || addr == 0x068F8000 || known_front_buffers_.count(addr) != 0;
  }
  void* vs_constants_mapped_ = nullptr;
  void* ps_constants_mapped_ = nullptr;
  void* shared_constants_mapped_ = nullptr;
  float* vb_mapped_ = nullptr;
  // The one descriptor set every pipeline reads (set 0): bindings 0-2 the
  // vertex, pixel and shared constants (uniform buffers at each draw's dynamic
  // offsets), 3-5 sixteen 2D, layered and cube textures, 6 sixteen samplers -
  // the shader's sampler slots. One per distinct set of slots, cached.
  plume::RenderDescriptorSetBuilder draw_set_builder_;
  struct DrawBindings {
    uint32_t tex2d[16] = {};
    uint32_t layered[16] = {};
    uint32_t cube[16] = {};
    uint32_t sampler[16] = {};
  };
  struct DrawSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t last_used = 0;
  };
  std::unordered_map<uint64_t, DrawSet> draw_sets_;
  uint64_t draw_sets_made_ = 0;
  plume::RenderDescriptorSet* DrawSetFor(const DrawBindings& bindings);
  void TrimDrawSets();

  std::array<plume::RenderInputSlot, 1> input_slots_{};
  std::array<plume::RenderInputElement, 32> input_elements_{};
  plume::RenderVertexBufferView vb_view_{};

  mutable std::shared_mutex vfetch_mutex_;
  std::unordered_map<uint64_t, std::vector<VfetchAttr>> vfetch_by_shader_;
  std::unordered_map<uint64_t, std::unique_ptr<ResolvedShaderVfetch>> resolved_vfetch_by_shader_;

  const ResolvedShaderVfetch* FindResolvedVfetch(uint64_t vs_hash) const {
    std::shared_lock lock(vfetch_mutex_);
    auto it = resolved_vfetch_by_shader_.find(vs_hash);
    return it != resolved_vfetch_by_shader_.end() ? it->second.get() : nullptr;
  }

  std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash>
      pipelines_;
  // The vertex layout each of the title's pipelines reads: the input
  // locations its vertex shader declares (bit n = location n), packed in
  // order. A pipeline not in here reads the full 20-location layout.
  std::unordered_map<const plume::RenderPipeline*, uint32_t> pipeline_layouts_;
  // The layout the vertices being filled are written in, for FillVertices.
  uint32_t fill_layout_mask_ = 0;
  // Copies `count` vertices from the full layout into `dst` in the layout
  // fill_layout_mask_ names.
  void CopyVerticesOut(float* dst, const float* staged, uint32_t count) const;
  // XERENGE_PACK_CHECK=1: every packed copy checked against the staging buffer
  // as it is made, its bytes hashed, and the hashes checked again when the
  // frame is done - a packed draw whose vertices changed in between was
  // overwritten by another. And the place each draw is bound at is checked
  // against the place its vertices were written to.
  struct PackedWrite {
    const float* dst = nullptr;
    size_t bytes = 0;
    uint64_t hash = 0;
    uint64_t vs = 0;
    uint32_t count = 0;
  };
  mutable std::vector<PackedWrite> packed_writes_;
  mutable const float* last_packed_dst_ = nullptr;
  uint64_t fill_vs_hash_ = 0;
  void CheckPackedWrites();
  std::unordered_map<uint64_t, std::unique_ptr<GuestHostTexture>> guest_textures_;
  uint32_t next_bindless_ = 1;
};

// Tells the vertex/constant writer trap (plume_draw.cpp) where the Direct3D device is.
void NoteD3DDeviceForTrap(uint32_t device_guest);

}  // namespace rex::plume_renderer
