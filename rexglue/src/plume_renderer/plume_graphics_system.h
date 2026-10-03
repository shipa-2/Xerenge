/**
 * @file        plume_renderer/plume_graphics_system.h
 * @brief       Plume GPU backend: direct-present swapchain (phase 1).
 *
 * volk lives inside librexgpu-plume.so (via static plume), not in the host
 * executable, so the Xenia-derived xenos plugin path stays unaffected.
 */
#pragma once

#include <rex/ui/renderdoc_api.h>

#include <array>
#include <deque>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "plume_renderer/plume_draw.h"

namespace plume {
class RenderDevice;
class VulkanInterface;
}  // namespace plume

namespace rex::plume_renderer {

class PlumeSwapchain;

class PlumeGraphicsSystem final : public system::IGraphicsSystem {
 public:
  PlumeGraphicsSystem();
  ~PlumeGraphicsSystem() override;

  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                       system::KernelState* kernel_state) override;
  bool has_presentation() const override;

  void AttachPresentationWindow(ui::Window* window) override;
  bool uses_direct_presentation() const override;
  void PresentGuestFrame(uint32_t width, uint32_t height) override;
  uint32_t CreateGuestShader(const void* shader_container, bool pixel_shader) override;
  void SubmitGuestDrawVerticesUP(uint32_t primitive_type, uint32_t vertex_count,
                               uint32_t vertex_stride, uint32_t data_guest_va) override;
  void NotifyGuestDrawIndexed(uint32_t index_count) override;
  void BindGuestD3DDevice(uint32_t device_guest) override;
  bool PresentedRecently() const override;
  void NoteGuestClear(uint32_t flags, float depth, uint32_t stencil) override;
  void NoteGuestClearColor(uint32_t flags, const float color[4], bool whole_target,
                           float depth) override;
  void NoteGuestFrameEnd() override;
  bool TakeCallTraceRequest() override { return call_trace_requested_.exchange(false); }
  void NoteGuestResolve(uint32_t flags, uint32_t dest_physical, uint32_t source_width,
                        uint32_t source_height, uint32_t face, bool cube) override;
  void PushMarker(GuestDrawSnapshot marker);
  void NoteShaderLoad(uint32_t object, bool pixel_shader, uint32_t microcode,
                      uint32_t size) override;
  bool ResolveShaderObject(uint32_t object_guest, bool pixel_shader);
  void RegisterGuestShaderObject(uint32_t object_guest, const void* shader_container,
                                 bool pixel_shader) override;
  void NoteGuestDraw(uint32_t primitive_type, uint32_t index_count,
                     const GuestDrawBuffers& buffers) override;

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  void Shutdown() override;

 private:
  bool InitializePlumeDevice();
  void PresentClearColorOnUiThread(uint32_t guest_width, uint32_t guest_height);
  void PublishDrawSnapshot(uint32_t prim_type, uint32_t source_select, uint32_t num_indices);
  uint32_t FindD3DDevice() const;
  bool DeviceLooksValid(uint32_t device_guest) const;
  void PullDeviceConstants(uint32_t device_guest);
  void RequestVideoCapture(const GuestDrawSnapshot& snap);
  void StartVideoWorker();
  void StopVideoWorker();
  void StartPresentWorker();
  void StopPresentWorker();
  void StartVsyncWorker();
  void StopVsyncWorker();
  void MarkVblank();
  // Holds the title's swap to one per vblank, as the console's flip does. The
  // title has no limiter of its own and ran at whatever the encoder managed -
  // 75 frames a second in the race - with its game running fast to match.
  void PaceSwapToVblank();

  // A resolve the guest asked for: copy what was rendered into one of its own
  // textures at dest_base, which later draws then sample. Requests are queued
  // on the command processor thread and carried out where the rendered image
  // exists, at present time.
  struct PendingResolve {
    uint32_t dest_base = 0;
    uint32_t copy_control = 0;
    uint32_t dest_info = 0;
    // RB_COPY_DEST_PITCH: the size the guest expects the copy to be, which is
    // its render resolution rather than the size of this host's window.
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
  };
  std::vector<PendingResolve> TakePendingResolves();

  void PumpCommandProcessor();
  void DispatchCpInterrupts(uint32_t cpu_mask);
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  static uint32_t ReadRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr,
                                 uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
  void ConsumeRingBuffer();
  void WriteReadPointerWriteback();
  void PublishGuestGpuProgress();
  void PublishSecondaryPosition(uint32_t block);
  void ExecutePrimaryRing();
  bool ExecutePm4Buffer(uint32_t base_phys, uint32_t start_index, uint32_t end_index,
                        uint32_t wrap_mask, bool wrapping, int depth);
  bool Pm4ReadDword(uint32_t base_phys, uint32_t& index, uint32_t end_index, uint32_t wrap_mask,
                    bool wrapping, uint32_t& out);
  void Pm4WritePhysical(uint32_t phys_addr, uint32_t value);
  uint32_t Pm4ReadPhysical(uint32_t phys_addr);
  void Pm4StoreRegister(uint32_t index, uint32_t value);
  uint64_t HashGuestPhysical(uint32_t phys_addr, uint32_t byte_count);
  uint64_t HashPm4Span(uint32_t base_phys, uint32_t start_index, uint32_t dword_count,
                       uint32_t wrap_mask, bool wrapping);
  void BindLoadedShader(uint32_t shader_type, uint64_t microcode_hash, uint32_t byte_size,
                        const uint8_t* bytes = nullptr);
  void DumpMicrocode(uint32_t shader_type, uint64_t microcode_hash, const uint8_t* bytes,
                     uint32_t byte_size);

  ui::WindowedAppContext* app_context_ = nullptr;
  ui::Window* window_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;

  std::unique_ptr<plume::VulkanInterface> render_interface_;
  std::unique_ptr<plume::RenderDevice> render_device_;
  std::unique_ptr<PlumeSwapchain> swapchain_;
  std::unique_ptr<PlumeDrawContext> draw_context_;
  std::vector<std::string> device_names_;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  uint32_t pending_cp_cpu_mask_ = 0;

  system::object_ref<system::XHostThread> vsync_worker_thread_;
  std::atomic<bool> vsync_worker_running_{false};
  std::atomic<uint32_t> frame_counter_{0};
  std::atomic<bool> call_trace_requested_{false};
  std::atomic<uint64_t> vblank_count_{0};
  uint64_t last_swap_vblank_ = 0;
  std::mutex vblank_mutex_;
  std::condition_variable vblank_cv_;

  std::mutex present_mutex_;
  std::mutex snapshot_mutex_;
  GuestDrawSnapshot pending_video_;
  std::vector<GuestDrawSnapshot> last_overlays_;
  std::atomic<uint64_t> last_video_key_{0};
  std::chrono::steady_clock::time_point last_video_capture_{};
  uint32_t no_video_presents_ = 0;
  // Consecutive frames turned away by VideoFrameWeakerThan. That test compares
  // against the frame already on screen, so without a bound it can latch and
  // freeze the picture - see where it is used.
  uint32_t video_frames_rejected_ = 0;
  bool video_seen_ = false;
  bool presented_once_ = false;
  // Presents in a row that found no Swap of the title's in the ring (see
  // PresentClearColorOnUiThread): a frame still being drawn is kept back.
  uint32_t presents_without_frame_end_ = 0;
  // The present's draw lists, reused from frame to frame (PresentClearColorOnUiThread).
  std::vector<GuestDrawSnapshot> present_batch_;
  std::vector<GuestDrawSnapshot> present_overlays_;
  // Frame generation (plume_framegen.h): the title's frame before the one being
  // presented, the frame drawn between them, and whether generating is held
  // off - the title waits for this thread, so when the frames between cost it
  // its own rate they are dropped for a while (longer each time).
  std::vector<GuestDrawSnapshot> framegen_previous_;
  std::vector<GuestDrawSnapshot> framegen_batch_;
  std::chrono::steady_clock::time_point framegen_last_arrival_{};
  std::chrono::steady_clock::time_point framegen_paused_until_{};
  double framegen_interval_us_ = 0.0;
  uint32_t framegen_backoff_s_ = 2;
  uint32_t FramesToGenerate();
  // The constant blocks of the last snapshot published (title thread only):
  // the next one shares them when its words are the same (AssignShared).
  SharedWords<1024> last_vs_constants_;
  SharedWords<1024> last_ps_constants_;
  SharedWords<192> last_fetch_constants_;
  // The same for the constants read from the Direct3D device per draw.
  SharedWords<1024> last_device_vs_constants_;
  SharedWords<1024> last_device_ps_constants_;
  SharedWords<192> last_device_fetch_constants_;
  // Menu frames issue upwards of 200 draws, ~160 of them valid overlays. At 96
  // this ring wrapped every frame and silently overwrote the draws issued
  // earliest - which is why rank badges and their labels went missing while
  // the banner, drawn last, survived. Sized with headroom; an overflow now
  // warns rather than quietly costing UI.
  // Scene frames with crashes/particles can exceed 2500 draws, so size with plenty of headroom.
  static constexpr uint32_t kDrawRingSize = 16384;
  std::vector<GuestDrawSnapshot> draw_ring_;
  uint32_t draw_ring_next_ = 0;
  uint32_t draw_ring_count_ = 0;

  struct GuestShaderSlot {
    uint64_t hash = 0;
    bool pixel_shader = false;
  };
  std::mutex guest_shader_mutex_;
  std::unordered_map<uint32_t, GuestShaderSlot> guest_shaders_;
  // Every microcode load seen at one physical address. The title reuses these
  // buffers, so an address alone names whichever shader happened to be loaded
  // there last - which is how scene geometry came to be unpacked with the
  // interface shader's layout. Keeping the length each load carried makes the
  // question answerable: re-hash that many bytes and see which load the
  // address still holds.
  struct MicrocodeLoad {
    uint32_t byte_count = 0;
    uint64_t hash = 0;
  };
  std::unordered_map<uint32_t, std::vector<MicrocodeLoad>> microcode_at_address_;
  // Shader objects recognised from their microcode rather than from a load:
  // (object << 32 | microcode pointer) -> translated shader, 0 for no match.
  std::unordered_map<uint64_t, uint64_t> recognised_by_content_;
  std::atomic<uint32_t> content_log_count_{0};
  // Microcode hashes whose vertex fetch layout has been registered from a
  // Direct3D shader load, so each is parsed once.
  std::unordered_set<uint64_t> layouts_from_loads_;
  std::unordered_set<uint64_t> dumped_from_loads_;
  std::atomic<uint64_t> loads_resolved_{0};
  std::atomic<uint64_t> loads_unknown_{0};
  // Shader objects bound but not yet identified, per stage, oldest first. The
  // title binds a shader and its Direct3D layer then puts the matching
  // microcode load into the ring, so the loads name these in the order they
  // were bound - but several can be bound between two loads, and keeping only
  // the most recent one lost the rest.
  std::deque<uint32_t> pending_shader_objects_[2];
  std::atomic<uint64_t> last_present_ms_{0};
  // Vblanks a swap is paced to (PaceSwapToVblank): 2 in Crash mode and with the
  // 30 fps lock. PresentedRecently's grace is that many frames longer.
  std::atomic<uint32_t> pace_interval_{1};
 public:
  // Front buffer address named by the last XE_SWAP packet.
  std::atomic<uint32_t> swap_frontbuffer_{0};
 private:
  // The opcode of the ring draw packet being published, 0 outside one.
  uint32_t ring_draw_opcode_ = 0;
  // Kept alive for the process: unloading RenderDoc under a live device is not
  // something it supports.
  std::unique_ptr<rex::ui::RenderDocAPI> renderdoc_api_;
  std::atomic<uint64_t> d3d_snapshots_seen_{0};
  std::atomic<uint64_t> d3d_snapshots_pushed_{0};
  std::atomic<uint64_t> ib_failures_{0};
  std::atomic<uint32_t> ib_longest_{0};
  std::atomic<uint64_t> draw_up_calls_{0};
  std::atomic<uint64_t> draw_up_vertices_{0};
  std::atomic<uint64_t> draw_indexed_calls_{0};
  std::atomic<uint64_t> draw_indexed_indices_{0};

  memory::Memory* memory_ = nullptr;
  uint32_t ring_buffer_ptr_ = 0;
  uint32_t ring_buffer_size_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;
  uint32_t write_ptr_index_ = 0;
  uint32_t read_ptr_index_ = 0;
  std::mutex resolve_mutex_;
  std::vector<PendingResolve> pending_resolves_;

  std::mutex ring_mutex_;
  std::array<uint32_t, 0x5003> gpu_registers_{};
  std::atomic<uint32_t> mmio_write_log_count_{0};
  std::atomic<uint32_t> pm4_log_count_{0};
  std::atomic<uint32_t> interrupt_log_count_{0};
  std::atomic<uint32_t> draw_detail_log_count_{0};
  std::atomic<uint64_t> shader_cache_hits_{0};
  std::atomic<uint64_t> shader_cache_misses_{0};
  uint64_t active_vs_hash_ = 0;
  uint64_t active_ps_hash_ = 0;
  std::unordered_set<uint64_t> seen_microcode_hashes_;
  std::atomic<uint32_t> d3d_device_guest_{0};
  std::array<uint32_t, 128> pm4_opcode_counts_{};
  std::atomic<uint32_t> pm4_type3_total_{0};
  std::atomic<uint32_t> set_constant_log_count_{0};
  std::atomic<uint32_t> device_const_log_count_{0};
  std::atomic<uint32_t> gpu_progress_log_count_{0};
  std::atomic<uint32_t> secondary_log_count_{0};
  std::atomic<uint32_t> progress_decline_log_count_{0};
  std::atomic<uint32_t> progress_state_log_count_{0};
  // Roughly half a second of vblanks with the position unmoved.
  static constexpr uint32_t kSecondaryStuckTicks = 30;
  uint32_t last_secondary_reserve_ = 0;
  uint32_t secondary_unchanged_ticks_ = 0;
  std::atomic<uint32_t> secondary_read_log_count_{0};
  // PM4 predication for tiled rendering; all-ones means run everything.
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t predicated_skips_ = 0;
  std::atomic<uint32_t> bin_log_count_{0};
  std::atomic<uint32_t> rptr_log_count_{0};
  std::atomic<uint32_t> shader_object_log_count_{0};
  std::atomic<uint32_t> d3d_draw_log_count_{0};
  std::atomic<uint32_t> d3d_big_draw_log_count_{0};
  // Draws the title issued through Direct3D, waiting for the ring's own draw
  // packet to say which shaders they use. Bounded: if the ring never carries
  // them, the queue must not grow without end.
  static constexpr size_t kMaxQueuedD3DDraws = 4096;
  std::mutex d3d_queue_mutex_;
  std::deque<GuestDrawBuffers> d3d_draw_queue_;
  GuestDrawBuffers pending_d3d_buffers_;
  uint64_t pending_d3d_shaders_[2] = {0, 0};
  std::atomic<uint32_t> d3d_unresolved_log_count_{0};
  std::atomic<uint32_t> slow_present_log_count_{0};
  bool present_in_flight_ = false;
  std::mutex present_flight_mutex_;
  std::condition_variable present_flight_cv_;
  std::atomic<uint32_t> slow_consume_log_count_{0};
  std::atomic<uint64_t> snapshot_pull_ns_{0};
  std::atomic<uint64_t> snapshot_copy_ns_{0};
  std::atomic<uint64_t> snapshot_draws_{0};
  std::atomic<uint64_t> snapshot_fill_ns_{0};
  std::atomic<uint64_t> snapshot_classify_ns_{0};
  std::atomic<uint64_t> snapshot_capture_ns_{0};
  std::atomic<uint64_t> video_captures_{0};
  std::atomic<uint64_t> video_distinct_frames_{0};
  uint64_t last_captured_key_ = 0;
  std::thread video_worker_;
  std::mutex video_request_mutex_;
  std::condition_variable video_request_cv_;
  GuestDrawSnapshot video_request_;
  bool video_request_pending_ = false;
  std::atomic<bool> video_worker_running_{false};
  std::thread present_worker_;
  std::mutex present_request_mutex_;
  std::condition_variable present_request_cv_;
  bool present_request_pending_ = false;
  uint32_t present_request_width_ = 0;
  uint32_t present_request_height_ = 0;
  std::atomic<bool> present_worker_running_{false};
  uint32_t last_secondary_cursor_ = 0;
  uint32_t secondary_lap_ = 0;
  bool secondary_cursor_seen_ = false;
};

}  // namespace rex::plume_renderer
