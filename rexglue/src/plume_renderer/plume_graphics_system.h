/**
 * @file        plume_renderer/plume_graphics_system.h
 * @brief       Plume GPU backend: direct-present swapchain (phase 1).
 *
 * volk lives inside librexgpu-plume.so (via static plume), not in the host
 * executable, so the Xenia-derived xenos plugin path stays unaffected.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
  void StartVsyncWorker();
  void StopVsyncWorker();
  void MarkVblank();
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  static uint32_t ReadRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* callback_context, uint32_t addr,
                                 uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
  void ConsumeRingBuffer();
  void WriteReadPointerWriteback();
  void ExecutePrimaryRing();
  bool ExecutePm4Buffer(uint32_t base_phys, uint32_t start_index, uint32_t end_index,
                        uint32_t wrap_mask, bool wrapping, int depth);
  bool Pm4ReadDword(uint32_t base_phys, uint32_t& index, uint32_t end_index, uint32_t wrap_mask,
                    bool wrapping, uint32_t& out);
  void Pm4WritePhysical(uint32_t phys_addr, uint32_t value);
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

  system::object_ref<system::XHostThread> vsync_worker_thread_;
  std::atomic<bool> vsync_worker_running_{false};
  std::atomic<uint32_t> frame_counter_{0};

  std::mutex present_mutex_;
  std::mutex snapshot_mutex_;
  GuestDrawSnapshot last_snapshot_;
  static constexpr uint32_t kDrawRingSize = 96;
  std::array<GuestDrawSnapshot, kDrawRingSize> draw_ring_{};
  uint32_t draw_ring_next_ = 0;
  uint32_t draw_ring_count_ = 0;

  struct GuestShaderSlot {
    uint64_t hash = 0;
    bool pixel_shader = false;
  };
  std::mutex guest_shader_mutex_;
  std::unordered_map<uint32_t, GuestShaderSlot> guest_shaders_;
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
  std::mutex ring_mutex_;
  std::array<uint32_t, 0x5003> gpu_registers_{};
  std::atomic<uint32_t> mmio_write_log_count_{0};
  std::atomic<uint32_t> pm4_log_count_{0};
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
};

}  // namespace rex::plume_renderer
