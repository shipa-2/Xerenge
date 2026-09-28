/**
 * @file        plume_renderer/plume_swapchain.h
 * @brief       Minimal plume swapchain: clear color + present (phase 1).
 */
#pragma once

#include "plume_renderer/plume_draw.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface_types.h>

struct SDL_Window;

namespace plume {
class RenderCommandFence;
class RenderCommandList;
class RenderQueryPool;
class RenderCommandQueue;
class RenderCommandSemaphore;
class RenderDevice;
class RenderFramebuffer;
class RenderSwapChain;
class RenderTexture;
}  // namespace plume

namespace rex::plume_renderer {

// Depth buffer format for the presentation framebuffer. Pipelines that enable
// depth have to declare the same one, so both sides read it from here.
inline constexpr plume::RenderFormat kPlumeDepthFormat = plume::RenderFormat::D32_FLOAT;

class PlumeSwapchain {
 public:
  PlumeSwapchain() = default;
  ~PlumeSwapchain();

  PlumeSwapchain(const PlumeSwapchain&) = delete;
  PlumeSwapchain& operator=(const PlumeSwapchain&) = delete;

  // `native_window` is the HWND on Windows, taken from the SDK: this library
  // links its own copy of SDL, which knows nothing of the SDK's windows.
  bool Initialize(plume::RenderDevice* device, SDL_Window* window, void* native_window);
  void Shutdown();

  bool IsReady() const { return ready_; }

  // `color` is the offscreen target the draws land on, not the swapchain
  // image. Guest draws go there so that a resolve - copying what was rendered
  // into one of the guest's own textures - can happen after the render pass
  // has ended, which is the only point at which such a copy is legal. The
  // finished image is then copied to the swapchain for presentation.
  using DrawEncodeFn = void (*)(void* context, plume::RenderCommandList* list, uint32_t width,
                                uint32_t height, plume::RenderTexture* color,
                                const RenderPassBreak& pass);
  // Called once the render pass is closed, with the same target.
  using ResolveFn = DrawEncodeFn;
  void ClearAndPresent(float r, float g, float b, float a, DrawEncodeFn encode = nullptr,
                       void* encode_context = nullptr, ResolveFn resolve = nullptr);

 private:
  void CreateFramebuffers();
  void ResizeIfNeeded();
  // The drawable size in pixels.
  void WindowPixelSize(uint32_t* width, uint32_t* height) const;

  plume::RenderDevice* device_ = nullptr;
  SDL_Window* window_ = nullptr;
  void* native_window_ = nullptr;

  std::unique_ptr<plume::RenderCommandQueue> command_queue_;
  std::unique_ptr<plume::RenderCommandList> command_list_;
  std::unique_ptr<plume::RenderCommandFence> submit_fence_;
  // The last frame's commands have been submitted and not yet waited for.
  // The wait happens at the start of the next frame, so the GPU works while
  // the title computes that frame instead of the title waiting for it.
  bool submit_pending_ = false;
  // A second set of the above, swapped with the first every frame when the
  // wait is left to the next frame (XERENGE_ASYNC_PRESENT): a frame is then
  // written while the GPU draws the one before, and what is waited for at a
  // frame's start is the frame before that, which used this set.
  std::unique_ptr<plume::RenderCommandList> spare_command_list_;
  // The copy onto the swap chain image and the present, recorded once the
  // image is acquired (see ClearAndPresent), one per set as well.
  std::unique_ptr<plume::RenderCommandList> present_list_;
  std::unique_ptr<plume::RenderCommandList> spare_present_list_;
  // The scene's own submit is fenced too: the present's fence covers only its
  // own batch.
  std::unique_ptr<plume::RenderCommandFence> scene_fence_;
  std::unique_ptr<plume::RenderCommandFence> spare_scene_fence_;
  bool scene_pending_ = false;
  bool spare_scene_pending_ = false;
  std::unique_ptr<plume::RenderCommandFence> spare_submit_fence_;
  std::unique_ptr<plume::RenderCommandSemaphore> spare_acquire_semaphore_;
  bool spare_submit_pending_ = false;
  // Two timestamps per frame, around its command list, one pool per set of
  // the above: how long the GPU spent on a frame, read once its fence is
  // waited for (reported as "GPU" in the host frame line).
  std::unique_ptr<plume::RenderQueryPool> query_pool_;
  std::unique_ptr<plume::RenderQueryPool> spare_query_pool_;
  bool query_written_ = false;
  bool spare_query_written_ = false;
  uint64_t gpu_busy_ns_ = 0;
  uint64_t gpu_busy_frames_ = 0;
  // Reads the finished frame's timestamps, if this set wrote any.
  void ReadGpuTime();
  // Waits for every frame still on the GPU.
  void WaitForFrames();
  // A small square from the middle of each frame, read back after the frame
  // has finished: a frame far darker than the ones before is reported with
  // what it held (XERENGE_FRAME_PROBE).
  std::unique_ptr<plume::RenderBuffer> probe_buffer_;
  void* probe_mapped_ = nullptr;
  bool probe_pending_ = false;
  double probe_average_ = 0.0;
  uint64_t probe_frames_ = 0;
  std::string probe_summary_;
  void CheckProbe();
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphore_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release_semaphores_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;

  // One depth buffer shared by every swapchain image. Only one frame is in
  // flight here - the queue is waited on at the end of each present - so they
  // cannot overlap. Without it the guest's depth test has nothing to work
  // against and 3D geometry is drawn in submission order.
  std::unique_ptr<plume::RenderTexture> depth_texture_;

  // Guest draws render here rather than straight into the swapchain image, so
  // that the frame can be copied out after the pass closes - both to satisfy
  // the guest's resolve requests and to reach the swapchain.
  std::unique_ptr<plume::RenderTexture> scene_texture_;
  // Render target 1, beside the scene target, when the title's targets are
  // followed (see PlumeSecondTargetEnabled).
  std::unique_ptr<plume::RenderTexture> scene_texture1_;
  std::unique_ptr<plume::RenderFramebuffer> scene_framebuffer_;
  // The size the frame is drawn at: the scene target's. The window's, unless
  // XERENGE_RENDER_RESOLUTION asks for another; the frame is then scaled onto
  // the window when it is presented.
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;

  uint32_t last_width_ = 0;
  uint32_t last_height_ = 0;
  bool ready_ = false;
};

}  // namespace rex::plume_renderer
