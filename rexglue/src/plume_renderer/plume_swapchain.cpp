/**
 * @file        plume_renderer/plume_swapchain.cpp
 * @brief       Minimal plume swapchain: clear color + present (phase 1).
 */
#include "plume_renderer/plume_swapchain.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <plume_render_interface.h>

#include <rex/logging.h>

namespace rex::plume_renderer {

namespace {

constexpr uint32_t kBufferCount = 2;
constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::B8G8R8A8_UNORM;

}  // namespace

PlumeSwapchain::~PlumeSwapchain() {
  Shutdown();
}

bool PlumeSwapchain::Initialize(plume::RenderDevice* device, SDL_Window* window) {
  Shutdown();

  if (!device || !window) {
    REXLOG_ERROR("plume: swapchain init requires device and SDL window");
    return false;
  }

  device_ = device;
  window_ = window;

  command_queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!command_queue_) {
    REXLOG_ERROR("plume: failed to create command queue");
    Shutdown();
    return false;
  }

  swap_chain_ = command_queue_->createSwapChain(
      plume::RenderSwapChainDesc(window_, kSwapchainFormat, kBufferCount));
  if (!swap_chain_) {
    REXLOG_ERROR("plume: failed to create swap chain");
    Shutdown();
    return false;
  }

  if (!swap_chain_->resize()) {
    REXLOG_ERROR("plume: initial swap chain resize failed");
    Shutdown();
    return false;
  }

  command_list_ = command_queue_->createCommandList();
  submit_fence_ = device_->createCommandFence();
  acquire_semaphore_ = device_->createCommandSemaphore();
  if (!command_list_ || !submit_fence_ || !acquire_semaphore_) {
    REXLOG_ERROR("plume: failed to create swapchain sync objects");
    Shutdown();
    return false;
  }

  CreateFramebuffers();

  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  last_width_ = uint32_t(std::max(pixel_width, 0));
  last_height_ = uint32_t(std::max(pixel_height, 0));

  ready_ = !framebuffers_.empty();
  if (ready_) {
    REXLOG_INFO("plume: swapchain ready ({}x{})", last_width_, last_height_);
  }
  return ready_;
}

void PlumeSwapchain::Shutdown() {
  ready_ = false;
  framebuffers_.clear();
  release_semaphores_.clear();
  acquire_semaphore_.reset();
  submit_fence_.reset();
  command_list_.reset();
  swap_chain_.reset();
  command_queue_.reset();
  device_ = nullptr;
  window_ = nullptr;
  last_width_ = 0;
  last_height_ = 0;
}

void PlumeSwapchain::CreateFramebuffers() {
  framebuffers_.clear();
  if (!swap_chain_ || !device_) {
    return;
  }

  const uint32_t texture_count = swap_chain_->getTextureCount();
  framebuffers_.reserve(texture_count);
  release_semaphores_.clear();
  release_semaphores_.reserve(texture_count);

  for (uint32_t i = 0; i < texture_count; ++i) {
    const plume::RenderTexture* color_attachment = swap_chain_->getTexture(i);
    plume::RenderFramebufferDesc fb_desc;
    fb_desc.colorAttachments = &color_attachment;
    fb_desc.colorAttachmentsCount = 1;
    fb_desc.depthAttachment = nullptr;
    framebuffers_.push_back(device_->createFramebuffer(fb_desc));
    release_semaphores_.push_back(device_->createCommandSemaphore());
  }
}

void PlumeSwapchain::ResizeIfNeeded() {
  if (!window_ || !swap_chain_) {
    return;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  const uint32_t width = uint32_t(std::max(pixel_width, 0));
  const uint32_t height = uint32_t(std::max(pixel_height, 0));
  if (width == last_width_ && height == last_height_) {
    return;
  }

  framebuffers_.clear();
  if (!swap_chain_->resize()) {
    REXLOG_WARN("plume: swap chain resize failed");
    ready_ = false;
    return;
  }

  last_width_ = width;
  last_height_ = height;
  CreateFramebuffers();
  ready_ = !framebuffers_.empty();
  REXLOG_INFO("plume: swapchain resized to {}x{}", last_width_, last_height_);
}

void PlumeSwapchain::ClearAndPresent(float r, float g, float b, float a, DrawEncodeFn encode,
                                     void* encode_context) {
  if (!ready_ || !swap_chain_ || !command_list_ || !command_queue_ || !acquire_semaphore_ ||
      !submit_fence_) {
    return;
  }

  ResizeIfNeeded();
  if (!ready_ || framebuffers_.empty()) {
    return;
  }

  uint32_t image_index = 0;
  if (!swap_chain_->acquireTexture(acquire_semaphore_.get(), &image_index)) {
    return;
  }

  if (image_index >= framebuffers_.size() || image_index >= release_semaphores_.size()) {
    return;
  }

  plume::RenderTexture* swapchain_texture = swap_chain_->getTexture(image_index);
  const plume::RenderFramebuffer* framebuffer = framebuffers_[image_index].get();
  plume::RenderCommandSemaphore* release_semaphore = release_semaphores_[image_index].get();

  command_list_->begin();
  command_list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                          plume::RenderTextureBarrier(swapchain_texture,
                                                      plume::RenderTextureLayout::COLOR_WRITE));
  command_list_->setFramebuffer(framebuffer);

  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();
  command_list_->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  command_list_->setScissors(plume::RenderRect(0, 0, width, height));
  command_list_->clearColor(0, plume::RenderColor(r, g, b, a));
  if (encode) {
    encode(encode_context, command_list_.get(), width, height);
  }
  command_list_->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(swapchain_texture,
                                                      plume::RenderTextureLayout::PRESENT));
  command_list_->end();

  const plume::RenderCommandList* cmd_list = command_list_.get();
  plume::RenderCommandSemaphore* wait_semaphore = acquire_semaphore_.get();
  command_queue_->executeCommandLists(&cmd_list, 1, &wait_semaphore, 1, &release_semaphore, 1,
                                      submit_fence_.get());
  swap_chain_->present(image_index, &release_semaphore, 1);
  command_queue_->waitForCommandFence(submit_fence_.get());
}

}  // namespace rex::plume_renderer
