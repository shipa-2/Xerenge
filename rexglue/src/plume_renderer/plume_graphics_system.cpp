/**
 * @file        plume_renderer/plume_graphics_system.cpp
 * @brief       Plume GPU backend implementation (phase 1).
 */
#include "plume_renderer/plume_graphics_system.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <xxhash.h>

#include <SDL3/SDL.h>

#include <plume_vulkan.h>

#include <algorithm>

#include <rex/chrono/clock.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "plume_renderer/plume_draw.h"
#include "plume_renderer/plume_shader_cache.h"
#include "plume_renderer/plume_swapchain.h"
#include "shader_cache.h"

namespace rex::plume_renderer {

PlumeGraphicsSystem::PlumeGraphicsSystem() = default;

PlumeGraphicsSystem::~PlumeGraphicsSystem() {
  Shutdown();
}

bool PlumeGraphicsSystem::InitializePlumeDevice() {
  if (render_device_) {
    return true;
  }

  render_interface_ = std::make_unique<plume::VulkanInterface>();
  if (!render_interface_ || !render_interface_->isValid()) {
    REXLOG_ERROR("plume: Vulkan interface failed to initialize");
    render_interface_.reset();
    return false;
  }

  device_names_ = render_interface_->getDeviceNames();
  REXLOG_INFO("plume: interface up, {} device(s)", device_names_.size());
  for (const std::string& name : device_names_) {
    REXLOG_INFO("plume:   {}", name);
  }

  render_device_ = render_interface_->createDevice("");
  if (!render_device_) {
    REXLOG_ERROR("plume: failed to create render device");
    render_interface_.reset();
    return false;
  }

  REXLOG_INFO("plume: render device ready");
  return true;
}

X_STATUS PlumeGraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  app_context_ = app_context;
  return X_STATUS_SUCCESS;
}

void PlumeGraphicsSystem::AttachPresentationWindow(ui::Window* window) {
  if (!window || !app_context_) {
    REXLOG_ERROR("plume: AttachPresentationWindow called without window or app context");
    return;
  }

  window_ = window;

  auto* sdl_window = static_cast<SDL_Window*>(window_->GetSDLWindowForVulkan());
  if (!sdl_window) {
    REXLOG_ERROR("plume: host window has no SDL surface for Vulkan");
    return;
  }

  if (!InitializePlumeDevice()) {
    return;
  }

  swapchain_ = std::make_unique<PlumeSwapchain>();
  if (!swapchain_->Initialize(render_device_.get(), sdl_window)) {
    REXLOG_ERROR("plume: swapchain initialization failed");
    swapchain_.reset();
    return;
  }

  draw_context_ = std::make_unique<PlumeDrawContext>();
  if (!draw_context_->Initialize(render_device_.get())) {
    REXLOG_ERROR("plume: draw context initialization failed");
    draw_context_.reset();
  }

  // Smoke present so --plume shows a visible clear color before the first VdSwap.
  app_context_->CallInUIThreadSynchronous([this]() { PresentClearColorOnUiThread(0, 0); });
}

X_STATUS PlumeGraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                            system::KernelState* kernel_state) {
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  if (!render_device_) {
    REXLOG_WARN("plume: SetupGuestGpu before presentation window; vsync worker deferred");
  }

  memory_ = kernel_state_ ? kernel_state_->memory() : nullptr;
  if (!memory_) {
    REXLOG_ERROR("plume: SetupGuestGpu has no guest memory; cannot map GPU MMIO");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Same window as xenos: D3D pokes CP_RB_WPTR / RB_BC_CONTROL here via REX_MM_*.
  // Without a registered range those loads return uninitialized values and WPTR
  // stores are dropped, so WaitForSpace spins forever after ring-buffer init.
  if (!memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                     &PlumeGraphicsSystem::ReadRegisterThunk,
                                     &PlumeGraphicsSystem::WriteRegisterThunk)) {
    REXLOG_ERROR("plume: failed to map GPU MMIO at 7FC80000");
    return X_STATUS_UNSUCCESSFUL;
  }

  REXLOG_INFO("plume: guest GPU wired (MMIO + instant ring consume; PM4/EDRAM bypassed)");
  StartVsyncWorker();
  return X_STATUS_SUCCESS;
}

bool PlumeGraphicsSystem::has_presentation() const {
  return swapchain_ && swapchain_->IsReady();
}

bool PlumeGraphicsSystem::uses_direct_presentation() const {
  return true;
}

uint32_t PlumeGraphicsSystem::CreateGuestShader(const void* shader_container, bool pixel_shader) {
  if (!shader_container || !kernel_state_) {
    return 0;
  }

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  const uint64_t hash = cache.HashShaderContainer(shader_container);
  if (hash == 0) {
    return 0;
  }

  plume::RenderShader* host_shader = cache.GetOrCreateShader(hash);
  (void)host_shader;

  auto* memory = kernel_state_->memory();
  if (!memory) {
    return 0;
  }

  const uint32_t guest_object = memory->SystemHeapAlloc(24, 16);
  if (guest_object == 0) {
    return 0;
  }
  memory->Zero(guest_object, 24);
  if (auto* ref_count = memory->TranslateVirtual<rex::be_u32*>(guest_object + 4)) {
    *ref_count = rex::be_u32(1);
  }

  {
    std::lock_guard lock(guest_shader_mutex_);
    guest_shaders_[guest_object] = GuestShaderSlot{hash, pixel_shader};
  }

  static std::atomic<uint32_t> log_count{0};
  const uint32_t n = log_count.fetch_add(1);
  if (n < 16) {
    REXLOG_INFO("plume: CreateGuestShader {} {:08X} hash={:016X} ({})",
                pixel_shader ? "PS" : "VS", guest_object, hash,
                host_shader ? "cache hit" : "cache miss");
  } else if (!host_shader && n < 20) {
    REXLOG_WARN("plume: shader cache miss hash={:016X}", hash);
  }

  return guest_object;
}

void PlumeGraphicsSystem::SubmitGuestDrawVerticesUP(uint32_t primitive_type, uint32_t vertex_count,
                                                  uint32_t vertex_stride,
                                                  uint32_t data_guest_va) {
  (void)primitive_type;
  (void)data_guest_va;
  const uint64_t call_index = draw_up_calls_.fetch_add(1) + 1;
  const uint64_t total_vertices = draw_up_vertices_.fetch_add(vertex_count) + vertex_count;
  if (call_index == 1 || (call_index % 200) == 0) {
    REXLOG_INFO("plume: DrawVerticesUP call #{} (+{} verts stride={}, {} total verts)", call_index,
                vertex_count, vertex_stride, total_vertices);
  }
}

void PlumeGraphicsSystem::NotifyGuestDrawIndexed(uint32_t index_count) {
  const uint64_t call_index = draw_indexed_calls_.fetch_add(1) + 1;
  const uint64_t total_indices = draw_indexed_indices_.fetch_add(index_count) + index_count;
  if (call_index == 1 || (call_index % 5000) == 0) {
    REXLOG_INFO("plume: PM4 DRAW #{} (+{} indices, {} total) vs={:016X} ps={:016X} cache {}/{}",
                call_index, index_count, total_indices, active_vs_hash_, active_ps_hash_,
                shader_cache_hits_.load(std::memory_order_relaxed),
                shader_cache_misses_.load(std::memory_order_relaxed));
  }
}

void PlumeGraphicsSystem::BindGuestD3DDevice(uint32_t device_guest) {
  if (device_guest >= 0x10000000 && device_guest < 0x80000000 && (device_guest & 0xF) == 0) {
    d3d_device_guest_.store(device_guest, std::memory_order_relaxed);
  }
}

namespace {

constexpr uint32_t kD3DRingOffset = 0x28;
constexpr uint32_t kD3DFetchOffset = 0x400;
constexpr uint32_t kD3DVsFloatOffset = 0x700;
constexpr uint32_t kD3DPsFloatOffset = 0x1700;
constexpr uint32_t kD3DVsBoolOffset = 0x2700;
constexpr uint32_t kD3DPsBoolOffset = 0x2710;
constexpr uint32_t kD3DDeviceMinBytes = 0x2800;

bool RangeReadable(memory::Memory* memory, uint32_t guest_va, uint32_t bytes) {
  if (!memory || guest_va < 0x10000000 || guest_va >= 0x80000000 || bytes == 0) {
    return false;
  }
  const uint32_t last = guest_va + bytes - 1;
  if (last < guest_va || last >= 0x80000000) {
    return false;
  }
  auto* heap = memory->LookupHeap(guest_va);
  if (!heap || memory->LookupHeap(last) != heap) {
    return false;
  }
  return heap->QueryRangeAccess(guest_va, last) != memory::PageAccess::kNoAccess;
}

void CopyBeDwords(memory::Memory* memory, uint32_t* dst, uint32_t guest_va, uint32_t count) {
  if (!memory || !dst || count == 0) {
    return;
  }
  const uint32_t bytes = count * 4;
  if (!RangeReadable(memory, guest_va, bytes)) {
    return;
  }
  const uint8_t* src = memory->TranslateVirtual<const uint8_t*>(guest_va);
  for (uint32_t i = 0; i < count; ++i) {
    dst[i] = rex::memory::load_and_swap<uint32_t>(src + i * 4);
  }
}

}  // namespace

bool PlumeGraphicsSystem::DeviceLooksValid(uint32_t device_guest) const {
  if (!memory_ || !RangeReadable(memory_, device_guest, kD3DDeviceMinBytes)) {
    return false;
  }
  const uint8_t* p = memory_->TranslateVirtual<const uint8_t*>(device_guest);
  const uint32_t fc0 = rex::memory::load_and_swap<uint32_t>(p + kD3DFetchOffset);
  const uint32_t fc1 = rex::memory::load_and_swap<uint32_t>(p + kD3DFetchOffset + 4);
  if (gpu_registers_[0x4800] != 0 && fc0 == gpu_registers_[0x4800] &&
      fc1 == gpu_registers_[0x4801]) {
    return true;
  }
  const uint32_t ring = rex::memory::load_and_swap<uint32_t>(p + kD3DRingOffset);
  return ring_buffer_ptr_ != 0 && (ring & 0x1FFFFFFF) == (ring_buffer_ptr_ & 0x1FFFFFFF);
}

uint32_t PlumeGraphicsSystem::FindD3DDevice() const {
  const uint32_t cached = d3d_device_guest_.load(std::memory_order_relaxed);
  return DeviceLooksValid(cached) ? cached : 0;
}

void PlumeGraphicsSystem::PullDeviceConstants(uint32_t device_guest) {
  if (!memory_ || !DeviceLooksValid(device_guest)) {
    return;
  }
  CopyBeDwords(memory_, &gpu_registers_[0x4000], device_guest + kD3DVsFloatOffset, 1024);
  CopyBeDwords(memory_, &gpu_registers_[0x4400], device_guest + kD3DPsFloatOffset, 1024);
  CopyBeDwords(memory_, &gpu_registers_[0x4800], device_guest + kD3DFetchOffset, 192);
  CopyBeDwords(memory_, &gpu_registers_[0x4900], device_guest + kD3DVsBoolOffset, 4);
  CopyBeDwords(memory_, &gpu_registers_[0x4904], device_guest + kD3DPsBoolOffset, 4);

  const uint32_t n = device_const_log_count_.fetch_add(1);
  if (n < 8) {
    const float* vs0 = reinterpret_cast<const float*>(&gpu_registers_[0x4000]);
    const float* ps0 = reinterpret_cast<const float*>(&gpu_registers_[0x4400]);
    REXLOG_INFO(
        "plume: D3D device {:08X} vs0=({:.4f},{:.4f},{:.4f},{:.4f}) "
        "ps0=({:.4f},{:.4f},{:.4f},{:.4f}) fetch0={:08X}",
        device_guest, vs0[0], vs0[1], vs0[2], vs0[3], ps0[0], ps0[1], ps0[2], ps0[3],
        gpu_registers_[0x4800]);
  }
}

void PlumeGraphicsSystem::PublishDrawSnapshot(uint32_t prim_type, uint32_t source_select,
                                              uint32_t num_indices) {
  const uint32_t device = FindD3DDevice();
  if (device) {
    PullDeviceConstants(device);
  }

  GuestDrawSnapshot snap;
  snap.vs_hash = active_vs_hash_;
  snap.ps_hash = active_ps_hash_;
  snap.prim_type = prim_type;
  snap.source_select = source_select;
  snap.num_indices = num_indices;
  std::memcpy(snap.vs_constants.data(), &gpu_registers_[0x4000],
              snap.vs_constants.size() * sizeof(uint32_t));
  std::memcpy(snap.ps_constants.data(), &gpu_registers_[0x4400],
              snap.ps_constants.size() * sizeof(uint32_t));
  std::memcpy(snap.fetch_constants.data(), &gpu_registers_[0x4800],
              snap.fetch_constants.size() * sizeof(uint32_t));
  snap.vs_bool = gpu_registers_[0x4900];
  snap.ps_bool = gpu_registers_[0x4904];
  snap.vte_cntl = gpu_registers_[0x2206];
  auto bits_to_float = [](uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  snap.vport_xscale = bits_to_float(gpu_registers_[0x210F]);
  snap.vport_xoffset = bits_to_float(gpu_registers_[0x2110]);
  snap.vport_yscale = bits_to_float(gpu_registers_[0x2111]);
  snap.vport_yoffset = bits_to_float(gpu_registers_[0x2112]);
  snap.vport_zscale = bits_to_float(gpu_registers_[0x2113]);
  snap.vport_zoffset = bits_to_float(gpu_registers_[0x2114]);
  snap.valid = snap.vs_hash != 0 && snap.ps_hash != 0 && snap.num_indices != 0;
  std::lock_guard lock(snapshot_mutex_);
  last_snapshot_ = snap;
  if (snap.valid && num_indices >= 3) {
    draw_ring_[draw_ring_next_] = snap;
    draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
    if (draw_ring_count_ < kDrawRingSize) {
      ++draw_ring_count_;
    }
  }
}

void PlumeGraphicsSystem::PresentGuestFrame(uint32_t width, uint32_t height) {
  if (!app_context_ || !swapchain_ || !swapchain_->IsReady()) {
    return;
  }

  frame_counter_.fetch_add(1, std::memory_order_relaxed);

  if (app_context_->CallInUIThreadSynchronous(
          [this, width, height]() { PresentClearColorOnUiThread(width, height); })) {
    return;
  }

  // Fallback if we're already on the UI thread.
  PresentClearColorOnUiThread(width, height);
}

void PlumeGraphicsSystem::PresentClearColorOnUiThread(uint32_t guest_width,
                                                        uint32_t guest_height) {
  (void)guest_width;
  (void)guest_height;

  if (!swapchain_ || !swapchain_->IsReady()) {
    return;
  }

  std::lock_guard lock(present_mutex_);

  std::vector<GuestDrawSnapshot> batch;
  {
    std::lock_guard snap_lock(snapshot_mutex_);
    if (draw_ring_count_ != 0) {
      const uint32_t start = (draw_ring_next_ + kDrawRingSize - draw_ring_count_) % kDrawRingSize;
      batch.reserve(draw_ring_count_);
      for (uint32_t i = 0; i < draw_ring_count_; ++i) {
        batch.push_back(draw_ring_[(start + i) % kDrawRingSize]);
      }
      draw_ring_count_ = 0;
      draw_ring_next_ = 0;
    } else if (last_snapshot_.valid) {
      batch.push_back(last_snapshot_);
    }
  }

  struct EncodeCtx {
    PlumeDrawContext* draw = nullptr;
    memory::Memory* memory = nullptr;
    const std::vector<GuestDrawSnapshot>* draws = nullptr;
  } ctx{draw_context_.get(), memory_, &batch};

  auto encode = [](void* raw, plume::RenderCommandList* list, uint32_t width, uint32_t height) {
    auto* encode_ctx = static_cast<EncodeCtx*>(raw);
    if (encode_ctx->draw && encode_ctx->draws) {
      encode_ctx->draw->EncodeDraws(list, *encode_ctx->draws, encode_ctx->memory, width, height);
    }
  };

  swapchain_->ClearAndPresent(0.03f, 0.04f, 0.07f, 1.0f, encode, &ctx);
}

void PlumeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXLOG_INFO("plume: SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void PlumeGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  std::lock_guard lock(ring_mutex_);
  ring_buffer_ptr_ = ptr;
  ring_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  write_ptr_index_ = 0;
  read_ptr_index_ = 0;
  gpu_registers_[0x01C4] = 0;
  gpu_registers_[0x01C5] = 0;
  REXLOG_INFO("plume: InitializeRingBuffer({:08X}, size_log2 {} -> {} bytes)", ptr, size_log2,
              ring_buffer_size_);
}

void PlumeGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  std::lock_guard lock(ring_mutex_);
  read_ptr_writeback_ptr_ = ptr;
  REXLOG_INFO("plume: EnableReadPointerWriteBack({:08X}, block_size_log2 {})", ptr,
              block_size_log2);
  WriteReadPointerWriteback();
}

void PlumeGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  uint32_t title_id, bool blocking) {
  (void)cache_root;
  (void)blocking;

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  cache.SetDevice(render_device_.get());
  cache.LogSummary();

  if (!cache.IsAvailable()) {
    return;
  }

  REXLOG_INFO("plume: warming shader cache for title {:08X}...", title_id);
  cache.WarmAll();
}

void PlumeGraphicsSystem::StartVsyncWorker() {
  if (vsync_worker_thread_ || !kernel_state_) {
    return;
  }

  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode{};
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        const double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        const uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        const uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();

        while (vsync_worker_running_) {
          const uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          const uint64_t interval_ticks = vsync_interval_ticks;
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  vsync_worker_thread_->set_name("GPU VSync (plume)");
  vsync_worker_thread_->Create();
}

void PlumeGraphicsSystem::StopVsyncWorker() {
  if (!vsync_worker_thread_) {
    return;
  }
  vsync_worker_running_ = false;
  vsync_worker_thread_->Wait(0, 0, 0, nullptr);
  vsync_worker_thread_.reset();
}

void PlumeGraphicsSystem::MarkVblank() {
  {
    std::lock_guard lock(ring_mutex_);
    ConsumeRingBuffer();
  }
  // Do not present from the vsync worker: CallInUIThreadSynchronous blocks
  // vblank interrupts, and Vulkan present racing createShader from the CP
  // thread corrupts guest state (log 158: write to 0x0).
  DispatchInterruptCallback(0, 2);
}

uint32_t PlumeGraphicsSystem::ReadRegisterThunk(void* /*ppc_context*/, void* callback_context,
                                                uint32_t addr) {
  return static_cast<PlumeGraphicsSystem*>(callback_context)->ReadRegister(addr);
}

void PlumeGraphicsSystem::WriteRegisterThunk(void* /*ppc_context*/, void* callback_context,
                                             uint32_t addr, uint32_t value) {
  static_cast<PlumeGraphicsSystem*>(callback_context)->WriteRegister(addr, value);
}

uint32_t PlumeGraphicsSystem::ReadRegister(uint32_t addr) {
  const uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x01C4: {  // CP_RB_RPTR — pretend the CP has consumed everything.
      std::lock_guard lock(ring_mutex_);
      return write_ptr_index_;
    }
    case 0x047F:  // CP_STAT
    case 0x039C:  // RBBM_STATUS2
      return 0;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode{};
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:  // interrupt status (vblank)
      return 1;
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      system::X_VIDEO_MODE video_mode{};
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      const uint32_t viewport_width =
          std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      const uint32_t viewport_height =
          std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      break;
  }

  if (r < gpu_registers_.size()) {
    return gpu_registers_[r];
  }
  return 0;
}

void PlumeGraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  const uint32_t n = mmio_write_log_count_.fetch_add(1);
  if (n < 8 || (n % 256) == 0) {
    REXLOG_INFO("plume: GPU MMIO write reg={:04X} value={:08X} (#{})", r, value, n + 1);
  }

  if (r < gpu_registers_.size()) {
    gpu_registers_[r] = value;
  }

  std::lock_guard lock(ring_mutex_);
  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      write_ptr_index_ = value;
      ConsumeRingBuffer();
      break;
    case 0x01C7:  // CP_RB_RPTR_WR
      read_ptr_index_ = value;
      WriteReadPointerWriteback();
      break;
    default:
      break;
  }
}

void PlumeGraphicsSystem::ConsumeRingBuffer() {
  ExecutePrimaryRing();
  if (0x01C4 < gpu_registers_.size()) {
    gpu_registers_[0x01C4] = read_ptr_index_;
  }
  WriteReadPointerWriteback();
}

void PlumeGraphicsSystem::WriteReadPointerWriteback() {
  if (!read_ptr_writeback_ptr_ || !memory_) {
    return;
  }
  auto* host = memory_->TranslatePhysical<uint8_t*>(read_ptr_writeback_ptr_);
  if (!host) {
    return;
  }
  rex::memory::store_and_swap<uint32_t>(host, read_ptr_index_);
}

bool PlumeGraphicsSystem::Pm4ReadDword(uint32_t base_phys, uint32_t& index, uint32_t end_index,
                                       uint32_t wrap_mask, bool wrapping, uint32_t& out) {
  if (!memory_) {
    return false;
  }
  if (wrapping) {
    if (index == end_index) {
      return false;
    }
  } else if (index >= end_index) {
    return false;
  }

  auto* host = memory_->TranslatePhysical<uint8_t*>(base_phys + (index << 2));
  if (!host) {
    return false;
  }
  out = rex::memory::load_and_swap<uint32_t>(host);
  index = wrapping ? ((index + 1) & wrap_mask) : (index + 1);
  return true;
}

void PlumeGraphicsSystem::Pm4WritePhysical(uint32_t phys_addr, uint32_t value) {
  if (!memory_) {
    return;
  }
  const uint32_t endian = phys_addr & 0x3;
  const uint32_t addr = phys_addr & ~uint32_t(0x3);
  auto* host = memory_->TranslatePhysical<uint8_t*>(addr);
  if (!host) {
    return;
  }
  const uint32_t swapped =
      rex::graphics::xenos::GpuSwap(value, static_cast<rex::graphics::xenos::Endian>(endian));
  rex::memory::store<uint32_t>(host, swapped);
}

void PlumeGraphicsSystem::Pm4StoreRegister(uint32_t index, uint32_t value) {
  if (index < gpu_registers_.size()) {
    gpu_registers_[index] = value;
  }
}

uint64_t PlumeGraphicsSystem::HashGuestPhysical(uint32_t phys_addr, uint32_t byte_count) {
  if (!memory_ || byte_count == 0) {
    return 0;
  }
  auto* host = memory_->TranslatePhysical<uint8_t*>(phys_addr);
  if (!host) {
    return 0;
  }
  return XXH3_64bits(host, byte_count);
}

uint64_t PlumeGraphicsSystem::HashPm4Span(uint32_t base_phys, uint32_t start_index,
                                          uint32_t dword_count, uint32_t wrap_mask, bool wrapping) {
  if (!memory_ || dword_count == 0) {
    return 0;
  }
  const uint32_t byte_count = dword_count * 4;
  if (!wrapping) {
    return HashGuestPhysical(base_phys + (start_index << 2), byte_count);
  }
  std::vector<uint8_t> tmp(byte_count);
  uint32_t index = start_index;
  for (uint32_t i = 0; i < dword_count; ++i) {
    auto* host = memory_->TranslatePhysical<uint8_t*>(base_phys + (index << 2));
    if (!host) {
      return 0;
    }
    std::memcpy(tmp.data() + (i << 2), host, 4);
    index = (index + 1) & wrap_mask;
  }
  return XXH3_64bits(tmp.data(), tmp.size());
}

void PlumeGraphicsSystem::DumpMicrocode(uint32_t shader_type, uint64_t microcode_hash,
                                        const uint8_t* bytes, uint32_t byte_size) {
  if (!bytes || byte_size == 0) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories("generated/ucode-dump", ec);
  char name[80];
  std::snprintf(name, sizeof(name), "%s_%016llX_%u.bin", shader_type == 0 ? "VS" : "PS",
                static_cast<unsigned long long>(microcode_hash), byte_size);
  const auto path = std::filesystem::path("generated/ucode-dump") / name;
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return;
  }
  out.write(reinterpret_cast<const char*>(bytes), std::streamsize(byte_size));
}

void PlumeGraphicsSystem::BindLoadedShader(uint32_t shader_type, uint64_t microcode_hash,
                                           uint32_t byte_size, const uint8_t* bytes) {
  if (microcode_hash == 0) {
    return;
  }

  if (bytes) {
    DumpMicrocode(shader_type, microcode_hash, bytes, byte_size);
  }

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  const ShaderMicrocodeEntry* micro = cache.FindByMicrocode(microcode_hash);
  const uint64_t shader_hash = micro ? micro->shaderHash : 0;
  if (shader_type == 0) {
    active_vs_hash_ = shader_hash ? shader_hash : microcode_hash;
  } else {
    active_ps_hash_ = shader_hash ? shader_hash : microcode_hash;
  }

  const bool first_seen = seen_microcode_hashes_.insert(microcode_hash).second;
  if (micro) {
    if (first_seen) {
      shader_cache_hits_.fetch_add(1, std::memory_order_relaxed);
      REXLOG_INFO("plume: IM_LOAD {} ucode={:016X} -> shader={:016X} ({} bytes)",
                  shader_type == 0 ? "VS" : "PS", microcode_hash, shader_hash, byte_size);
    }
    if (shader_type == 0 && bytes && draw_context_) {
      draw_context_->RegisterVsUcode(shader_hash, bytes, byte_size);
    }
    return;
  }

  if (first_seen) {
    shader_cache_misses_.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("plume: IM_LOAD {} ucode={:016X} cache MISS ({} bytes, unique #{})",
                shader_type == 0 ? "VS" : "PS", microcode_hash, byte_size,
                seen_microcode_hashes_.size());
  }
}

void PlumeGraphicsSystem::ExecutePrimaryRing() {
  if (!memory_ || ring_buffer_ptr_ == 0 || ring_buffer_size_ < 4) {
    read_ptr_index_ = write_ptr_index_;
    return;
  }
  const uint32_t dword_count = ring_buffer_size_ / 4;
  const uint32_t wrap_mask = dword_count - 1;
  if (!ExecutePm4Buffer(ring_buffer_ptr_, read_ptr_index_, write_ptr_index_, wrap_mask, true, 0)) {
    REXLOG_WARN("plume: PM4 primary walk failed; snapping rptr {:08X} -> {:08X}", read_ptr_index_,
                write_ptr_index_);
  }
  read_ptr_index_ = write_ptr_index_;
}

bool PlumeGraphicsSystem::ExecutePm4Buffer(uint32_t base_phys, uint32_t start_index,
                                           uint32_t end_index, uint32_t wrap_mask, bool wrapping,
                                           int depth) {
  using rex::graphics::xenos::PM4_COND_WRITE;
  using rex::graphics::xenos::PM4_DRAW_INDX;
  using rex::graphics::xenos::PM4_DRAW_INDX_2;
  using rex::graphics::xenos::PM4_DRAW_INDX_2_BIN;
  using rex::graphics::xenos::PM4_DRAW_INDX_BIN;
  using rex::graphics::xenos::PM4_EVENT_WRITE;
  using rex::graphics::xenos::PM4_EVENT_WRITE_SHD;
  using rex::graphics::xenos::PM4_IM_LOAD;
  using rex::graphics::xenos::PM4_IM_LOAD_IMMEDIATE;
  using rex::graphics::xenos::PM4_INDIRECT_BUFFER;
  using rex::graphics::xenos::PM4_INDIRECT_BUFFER_PFD;
  using rex::graphics::xenos::PM4_INTERRUPT;
  using rex::graphics::xenos::PM4_LOAD_ALU_CONSTANT;
  using rex::graphics::xenos::PM4_LOAD_CONSTANT_CONTEXT;
  using rex::graphics::xenos::PM4_ME_INIT;
  using rex::graphics::xenos::PM4_MEM_WRITE;
  using rex::graphics::xenos::PM4_NOP;
  using rex::graphics::xenos::PM4_REG_RMW;
  using rex::graphics::xenos::PM4_REG_TO_MEM;
  using rex::graphics::xenos::PM4_SET_CONSTANT;
  using rex::graphics::xenos::PM4_SET_CONSTANT2;
  using rex::graphics::xenos::PM4_SET_SHADER_CONSTANTS;
  using rex::graphics::xenos::PM4_WAIT_FOR_IDLE;
  using rex::graphics::xenos::PM4_WAIT_REG_MEM;

  if (depth > 4) {
    REXLOG_WARN("plume: PM4 IB nesting too deep");
    return false;
  }

  uint32_t index = start_index;
  uint32_t packets = 0;
  uint32_t dword_span = 0;
  if (wrapping) {
    if (start_index != end_index) {
      dword_span = (end_index - start_index) & wrap_mask;
    }
  } else if (end_index > start_index) {
    dword_span = end_index - start_index;
  }
  const uint32_t kMaxPackets = dword_span + 1;

  auto read_word = [&](uint32_t& out) -> bool {
    return Pm4ReadDword(base_phys, index, end_index, wrap_mask, wrapping, out);
  };

  auto skip_words = [&](uint32_t count) -> bool {
    uint32_t dummy = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (!read_word(dummy)) {
        return false;
      }
    }
    return true;
  };

  while (packets < kMaxPackets) {
    if (wrapping) {
      if (index == end_index) {
        break;
      }
    } else if (index >= end_index) {
      break;
    }

    uint32_t packet = 0;
    if (!read_word(packet)) {
      return false;
    }
    ++packets;

    if (packet == 0) {
      continue;
    }

    const uint32_t packet_type = packet >> 30;
    if (packet_type == 0x02) {
      continue;
    }

    if (packet_type == 0x00) {
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      const uint32_t base_index = packet & 0x7FFF;
      const bool write_one_reg = ((packet >> 15) & 0x1) != 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t data = 0;
        if (!read_word(data)) {
          return false;
        }
        Pm4StoreRegister(write_one_reg ? base_index : base_index + i, data);
      }
      continue;
    }

    if (packet_type == 0x01) {
      uint32_t data1 = 0;
      uint32_t data2 = 0;
      if (!read_word(data1) || !read_word(data2)) {
        return false;
      }
      Pm4StoreRegister(packet & 0x7FF, data1);
      Pm4StoreRegister((packet >> 11) & 0x7FF, data2);
      continue;
    }

    if (packet_type != 0x03) {
      REXLOG_WARN("plume: unknown PM4 type {:X} packet={:08X}", packet_type, packet);
      return false;
    }

    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (opcode < pm4_opcode_counts_.size()) {
      ++pm4_opcode_counts_[opcode];
    }
    const uint32_t type3_total = pm4_type3_total_.fetch_add(1) + 1;
    if (type3_total == 64 || type3_total == 256 || type3_total == 1024) {
      REXLOG_INFO("plume: PM4 type3 histogram after {} packets:", type3_total);
      for (uint32_t op = 0; op < pm4_opcode_counts_.size(); ++op) {
        if (pm4_opcode_counts_[op] != 0) {
          REXLOG_INFO("plume:   op={:02X} count={}", op, pm4_opcode_counts_[op]);
        }
      }
    }
    const uint32_t n = pm4_log_count_.fetch_add(1);
    if (n < 32) {
      REXLOG_INFO("plume: PM4 type3 op={:02X} count={} {}", opcode, count,
                  wrapping ? "ring" : "ib");
    }

    switch (opcode) {
      case PM4_INDIRECT_BUFFER:
      case PM4_INDIRECT_BUFFER_PFD: {
        uint32_t list_ptr = 0;
        uint32_t list_length = 0;
        if (count < 2 || !read_word(list_ptr) || !read_word(list_length)) {
          return false;
        }
        if (!skip_words(count - 2)) {
          return false;
        }
        list_ptr = rex::graphics::xenos::CpuToGpu(list_ptr);
        list_length &= 0xFFFFF;
        if (list_length > 0x10000) {
          REXLOG_WARN("plume: PM4 IB {:08X} dwords={} (clamped)", list_ptr, list_length);
          list_length = 0x10000;
        }
        if (list_length != 0) {
          if (n < 32) {
            REXLOG_INFO("plume: PM4 IB {:08X} dwords={}", list_ptr, list_length);
          }
          if (!ExecutePm4Buffer(list_ptr, 0, list_length, 0, false, depth + 1)) {
            return false;
          }
        }
        break;
      }
      case PM4_MEM_WRITE: {
        uint32_t write_addr = 0;
        if (count < 1 || !read_word(write_addr)) {
          return false;
        }
        for (uint32_t i = 0; i < count - 1; ++i) {
          uint32_t write_data = 0;
          if (!read_word(write_data)) {
            return false;
          }
          if (n < 32) {
            REXLOG_INFO("plume: PM4 MEM_WRITE {:08X}={:08X}", write_addr & ~uint32_t(0x3),
                        write_data);
          }
          Pm4WritePhysical(write_addr, write_data);
          write_addr += 4;
        }
        break;
      }
      case PM4_EVENT_WRITE_SHD: {
        uint32_t initiator = 0;
        uint32_t address = 0;
        uint32_t value = 0;
        if (count < 3 || !read_word(initiator) || !read_word(address) || !read_word(value)) {
          return false;
        }
        if (!skip_words(count - 3)) {
          return false;
        }
        const uint32_t data_value = ((initiator >> 31) & 0x1) ? 1u : value;
        if (n < 32) {
          REXLOG_INFO("plume: PM4 EVENT_WRITE_SHD {:08X}={:08X}", address & ~uint32_t(0x3),
                      data_value);
        }
        Pm4WritePhysical(address, data_value);
        break;
      }
      case PM4_REG_TO_MEM: {
        uint32_t reg_addr = 0;
        uint32_t mem_addr = 0;
        if (count < 2 || !read_word(reg_addr) || !read_word(mem_addr)) {
          return false;
        }
        if (!skip_words(count - 2)) {
          return false;
        }
        uint32_t reg_val = 0;
        if (reg_addr < gpu_registers_.size()) {
          reg_val = gpu_registers_[reg_addr];
        }
        Pm4WritePhysical(mem_addr, reg_val);
        break;
      }
      case PM4_COND_WRITE: {
        uint32_t wait_info = 0;
        uint32_t poll_addr = 0;
        uint32_t ref = 0;
        uint32_t mask = 0;
        uint32_t write_addr = 0;
        uint32_t write_data = 0;
        if (count < 6 || !read_word(wait_info) || !read_word(poll_addr) || !read_word(ref) ||
            !read_word(mask) || !read_word(write_addr) || !read_word(write_data)) {
          return false;
        }
        if (!skip_words(count - 6)) {
          return false;
        }
        // Fake GPU: treat as always matched so identifier/fence writes land.
        if (wait_info & 0x100) {
          Pm4WritePhysical(write_addr, write_data);
        } else {
          Pm4StoreRegister(write_addr, write_data);
        }
        break;
      }
      case PM4_REG_RMW: {
        uint32_t rmw_info = 0;
        uint32_t and_mask = 0;
        uint32_t or_mask = 0;
        if (count < 3 || !read_word(rmw_info) || !read_word(and_mask) || !read_word(or_mask)) {
          return false;
        }
        if (!skip_words(count - 3)) {
          return false;
        }
        const uint32_t reg = rmw_info & 0x1FFF;
        uint32_t value = (reg < gpu_registers_.size()) ? gpu_registers_[reg] : 0;
        if ((rmw_info >> 31) & 0x1) {
          const uint32_t src = and_mask & 0x1FFF;
          value &= (src < gpu_registers_.size()) ? gpu_registers_[src] : 0;
        } else {
          value &= and_mask;
        }
        if ((rmw_info >> 30) & 0x1) {
          const uint32_t src = or_mask & 0x1FFF;
          value |= (src < gpu_registers_.size()) ? gpu_registers_[src] : 0;
        } else {
          value |= or_mask;
        }
        Pm4StoreRegister(reg, value);
        break;
      }
      case PM4_SET_CONSTANT: {
        uint32_t offset_type = 0;
        if (count < 1 || !read_word(offset_type)) {
          return false;
        }
        const uint32_t type_index = offset_type & 0x7FF;
        const uint32_t type = (offset_type >> 16) & 0xFF;
        uint32_t base = type_index;
        switch (type) {
          case 0:
            base = type_index + 0x4000;
            break;
          case 1:
            base = type_index + 0x4800;
            break;
          case 2:
            base = type_index + 0x4900;
            break;
          case 3:
            base = type_index + 0x4908;
            break;
          case 4:
            base = type_index + 0x2000;
            break;
          default:
            if (!skip_words(count - 1)) {
              return false;
            }
            break;
        }
        if (type <= 4) {
          uint32_t first = 0;
          for (uint32_t i = 0; i < count - 1; ++i) {
            uint32_t data = 0;
            if (!read_word(data)) {
              return false;
            }
            if (i == 0) {
              first = data;
            }
            Pm4StoreRegister(base + i, data);
          }
          const uint32_t logged = set_constant_log_count_.fetch_add(1);
          if (logged < 24) {
            REXLOG_INFO(
                "plume: SET_CONSTANT type={} idx={:03X} extra={:04X} n={} base={:04X} first={:08X}",
                type, type_index, (offset_type & 0xFFFF) & ~uint32_t(0x7FF), count - 1, base, first);
          }
        }
        break;
      }
      case PM4_SET_CONSTANT2:
      case PM4_SET_SHADER_CONSTANTS: {
        uint32_t offset_type = 0;
        if (count < 1 || !read_word(offset_type)) {
          return false;
        }
        const uint32_t base = offset_type & 0xFFFF;
        for (uint32_t i = 0; i < count - 1; ++i) {
          uint32_t data = 0;
          if (!read_word(data)) {
            return false;
          }
          Pm4StoreRegister(base + i, data);
        }
        break;
      }
      case PM4_LOAD_ALU_CONSTANT: {
        uint32_t address = 0;
        uint32_t offset_type = 0;
        uint32_t size_dwords = 0;
        if (count < 3 || !read_word(address) || !read_word(offset_type) || !read_word(size_dwords)) {
          return false;
        }
        if (!skip_words(count - 3)) {
          return false;
        }
        address &= 0x3FFFFFFF;
        size_dwords &= 0xFFF;
        const uint32_t type_index = offset_type & 0x7FF;
        const uint32_t type = (offset_type >> 16) & 0xFF;
        uint32_t base = type_index;
        switch (type) {
          case 0:
            base = type_index + 0x4000;
            break;
          case 1:
            base = type_index + 0x4800;
            break;
          case 2:
            base = type_index + 0x4900;
            break;
          case 3:
            base = type_index + 0x4908;
            break;
          case 4:
            base = type_index + 0x2000;
            break;
          default:
            break;
        }
        if (type <= 4 && memory_) {
          for (uint32_t i = 0; i < size_dwords; ++i) {
            auto* host = memory_->TranslatePhysical<uint8_t*>(address + (i << 2));
            if (!host) {
              break;
            }
            Pm4StoreRegister(base + i, rex::memory::load_and_swap<uint32_t>(host));
          }
        }
        break;
      }
      case PM4_IM_LOAD: {
        uint32_t addr_type = 0;
        uint32_t start_size = 0;
        if (count < 2 || !read_word(addr_type) || !read_word(start_size)) {
          return false;
        }
        if (!skip_words(count - 2)) {
          return false;
        }
        const uint32_t shader_type = addr_type & 0x3;
        const uint32_t addr = addr_type & ~uint32_t(0x3);
        const uint32_t size_dwords = start_size & 0xFFFF;
        const uint32_t byte_count = size_dwords * 4;
        auto* host = memory_ ? memory_->TranslatePhysical<uint8_t*>(addr) : nullptr;
        BindLoadedShader(shader_type, HashGuestPhysical(addr, byte_count), byte_count, host);
        break;
      }
      case PM4_IM_LOAD_IMMEDIATE: {
        uint32_t dword0 = 0;
        uint32_t dword1 = 0;
        if (count < 2 || !read_word(dword0) || !read_word(dword1)) {
          return false;
        }
        const uint32_t shader_type = dword0 & 0x3;
        const uint32_t size_dwords = dword1 & 0xFFFF;
        const uint32_t payload = (count >= 2) ? (count - 2) : 0;
        const uint32_t hash_dwords = std::min(size_dwords, payload);
        const uint32_t byte_count = hash_dwords * 4;
        const uint64_t hash = HashPm4Span(base_phys, index, hash_dwords, wrap_mask, wrapping);
        const uint8_t* bytes = nullptr;
        std::vector<uint8_t> wrapped;
        if (!wrapping && memory_) {
          bytes = memory_->TranslatePhysical<uint8_t*>(base_phys + (index << 2));
        } else if (wrapping && byte_count != 0) {
          wrapped.resize(byte_count);
          uint32_t copy_index = index;
          for (uint32_t i = 0; i < hash_dwords; ++i) {
            auto* host = memory_->TranslatePhysical<uint8_t*>(base_phys + (copy_index << 2));
            if (!host) {
              wrapped.clear();
              break;
            }
            std::memcpy(wrapped.data() + (i << 2), host, 4);
            copy_index = (copy_index + 1) & wrap_mask;
          }
          if (!wrapped.empty()) {
            bytes = wrapped.data();
          }
        }
        BindLoadedShader(shader_type, hash, byte_count, bytes);
        if (!skip_words(payload)) {
          return false;
        }
        break;
      }
      case PM4_DRAW_INDX:
      case PM4_DRAW_INDX_2:
      case PM4_DRAW_INDX_BIN:
      case PM4_DRAW_INDX_2_BIN: {
        uint32_t remaining = count;
        if (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_BIN) {
          uint32_t viz = 0;
          if (remaining < 1 || !read_word(viz)) {
            return false;
          }
          --remaining;
        }
        uint32_t initiator = 0;
        if (remaining < 1 || !read_word(initiator)) {
          return false;
        }
        --remaining;
        Pm4StoreRegister(0x21FC, initiator);
        const uint32_t prim_type = initiator & 0x3F;
        const uint32_t source_select = (initiator >> 6) & 0x3;
        const uint32_t num_indices = initiator >> 16;
        if (source_select == 0 && remaining >= 2) {
          uint32_t dma_base = 0;
          uint32_t dma_size = 0;
          if (!read_word(dma_base) || !read_word(dma_size)) {
            return false;
          }
          remaining -= 2;
          Pm4StoreRegister(0x21FA, dma_base);
          Pm4StoreRegister(0x21FB, dma_size);
        }
        if (!skip_words(remaining)) {
          return false;
        }
        NotifyGuestDrawIndexed(num_indices ? num_indices : 1);
        PublishDrawSnapshot(prim_type, source_select, num_indices ? num_indices : 1);
        const uint32_t detail = draw_detail_log_count_.fetch_add(1);
        if (detail < 8) {
          REXLOG_INFO(
              "plume: DRAW op={:02X} initiator={:08X} prim={} src={} indices={} vs={:016X} "
              "ps={:016X}",
              opcode, initiator, prim_type, source_select, num_indices, active_vs_hash_,
              active_ps_hash_);
        }
        break;
      }
      case PM4_LOAD_CONSTANT_CONTEXT:
      case PM4_ME_INIT:
      case PM4_NOP:
      case PM4_WAIT_REG_MEM:
      case PM4_WAIT_FOR_IDLE:
      case PM4_INTERRUPT:
      case PM4_EVENT_WRITE:
      default:
        if (!skip_words(count)) {
          return false;
        }
        break;
    }
  }

  if (packets >= kMaxPackets) {
    REXLOG_WARN("plume: PM4 walk hit packet cap");
    return false;
  }
  return true;
}

void PlumeGraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_ || !function_dispatcher_) {
    return;
  }

  auto* thread = system::XThread::GetCurrentThread();
  if (!thread) {
    return;
  }

  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void PlumeGraphicsSystem::Shutdown() {
  StopVsyncWorker();

  if (swapchain_ && app_context_) {
    app_context_->CallInUIThreadSynchronous([this]() {
      std::lock_guard lock(present_mutex_);
      swapchain_.reset();
      draw_context_.reset();
    });
  } else {
    swapchain_.reset();
    draw_context_.reset();
  }

  render_device_.reset();
  render_interface_.reset();
  device_names_.clear();
  window_ = nullptr;
  app_context_ = nullptr;
  function_dispatcher_ = nullptr;
  kernel_state_ = nullptr;
  memory_ = nullptr;
  interrupt_callback_ = 0;
  interrupt_callback_data_ = 0;
  guest_shaders_.clear();
  seen_microcode_hashes_.clear();
  active_vs_hash_ = 0;
  active_ps_hash_ = 0;
  draw_up_calls_ = 0;
  draw_up_vertices_ = 0;
  draw_indexed_calls_ = 0;
  draw_indexed_indices_ = 0;
  shader_cache_hits_ = 0;
  shader_cache_misses_ = 0;
}

}  // namespace rex::plume_renderer
