/**
 * @file        plume_renderer/plume_swapchain.h
 * @brief       Minimal plume swapchain: clear color + present (phase 1).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct SDL_Window;

namespace plume {
class RenderCommandFence;
class RenderCommandList;
class RenderCommandQueue;
class RenderCommandSemaphore;
class RenderDevice;
class RenderFramebuffer;
class RenderSwapChain;
}  // namespace plume

namespace rex::plume_renderer {

class PlumeSwapchain {
 public:
  PlumeSwapchain() = default;
  ~PlumeSwapchain();

  PlumeSwapchain(const PlumeSwapchain&) = delete;
  PlumeSwapchain& operator=(const PlumeSwapchain&) = delete;

  bool Initialize(plume::RenderDevice* device, SDL_Window* window);
  void Shutdown();

  bool IsReady() const { return ready_; }

  using DrawEncodeFn = void (*)(void* context, plume::RenderCommandList* list, uint32_t width,
                                uint32_t height);
  void ClearAndPresent(float r, float g, float b, float a, DrawEncodeFn encode = nullptr,
                       void* encode_context = nullptr);

 private:
  void CreateFramebuffers();
  void ResizeIfNeeded();

  plume::RenderDevice* device_ = nullptr;
  SDL_Window* window_ = nullptr;

  std::unique_ptr<plume::RenderCommandQueue> command_queue_;
  std::unique_ptr<plume::RenderCommandList> command_list_;
  std::unique_ptr<plume::RenderCommandFence> submit_fence_;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphore_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release_semaphores_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;

  uint32_t last_width_ = 0;
  uint32_t last_height_ = 0;
  bool ready_ = false;
};

}  // namespace rex::plume_renderer
