/**
 * @file        plume_renderer/plume_swapchain.cpp
 * @brief       Minimal plume swapchain: clear color + present (phase 1).
 */
#include <chrono>
#include <cstdlib>
#include "plume_renderer/plume_swapchain.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <plume_render_interface.h>

#include <rex/logging.h>

namespace rex::plume_renderer {

namespace {

constexpr uint32_t kBufferCount = 2;
constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::B8G8R8A8_UNORM;

// The window as plume takes it: SDL's own where plume draws through SDL's Vulkan
// surface, the Win32 handle on Windows.
plume::RenderWindow NativeRenderWindow(SDL_Window* window) {
#ifdef _WIN32
  return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#else
  return window;
#endif
}

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
      plume::RenderSwapChainDesc(NativeRenderWindow(window_), kSwapchainFormat, kBufferCount));
  if (!swap_chain_) {
    REXLOG_ERROR("plume: failed to create swap chain");
    Shutdown();
    return false;
  }

  // XERENGE_UNLOCK_FPS: no vsync on the host side either (immediate present,
  // where the driver has it), so the frame rate is whatever the renderer
  // manages. The game's logic steps once per frame, so it runs fast with it.
  if (std::getenv("XERENGE_UNLOCK_FPS") != nullptr) {
    swap_chain_->setVsyncEnabled(false);
    REXLOG_WARN("plume: frame rate unlocked - the game runs faster than it should");
  }
  if (!swap_chain_->resize()) {
    REXLOG_ERROR("plume: initial swap chain resize failed");
    Shutdown();
    return false;
  }
  if (std::getenv("XERENGE_UNLOCK_FPS") != nullptr) {
    REXLOG_INFO("plume: swap chain vsync {}", swap_chain_->isVsyncEnabled()
                                                  ? "still on (FIFO or mailbox)"
                                                  : "off (immediate)");
  }

  command_list_ = command_queue_->createCommandList();
  submit_fence_ = device_->createCommandFence();
  acquire_semaphore_ = device_->createCommandSemaphore();
  if (!command_list_ || !submit_fence_ || !acquire_semaphore_) {
    REXLOG_ERROR("plume: failed to create swapchain sync objects");
    Shutdown();
    return false;
  }

  if (std::getenv("XERENGE_FRAME_PROBE") != nullptr) {
    probe_buffer_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(64 * 64 * 4));
    probe_mapped_ = probe_buffer_ ? probe_buffer_->map() : nullptr;
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
  if (submit_pending_ && command_queue_ && submit_fence_) {
    command_queue_->waitForCommandFence(submit_fence_.get());
  }
  submit_pending_ = false;
  ready_ = false;
  framebuffers_.clear();
  release_semaphores_.clear();
  acquire_semaphore_.reset();
  if (probe_buffer_ && probe_mapped_) {
    probe_buffer_->unmap();
  }
  probe_mapped_ = nullptr;
  probe_buffer_.reset();
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

  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();
  depth_texture_.reset();
  scene_framebuffer_.reset();
  scene_texture_.reset();
  scene_texture1_.reset();
  if (width != 0 && height != 0) {
    depth_texture_ = device_->createTexture(
        plume::RenderTextureDesc::DepthTarget(width, height, kPlumeDepthFormat));
    if (!depth_texture_) {
      REXLOG_ERROR("plume: could not create a {}x{} depth target; depth testing stays off", width,
                   height);
    }
    // Rendering the frame offscreen first is what makes the guest's resolves
    // possible, but it also changes how every frame reaches the screen, and
    // that has broken video playback more than once. Off unless asked for, so
    // the default path stays the one known to work.
    //
    // Following the title's render targets needs it: what is shown is then the
    // front buffer the frame was resolved into, copied over this target once
    // the pass is closed - and there is nowhere to copy it without one.
    static const bool scene_target_enabled = std::getenv("XERENGE_SCENE_TARGET") != nullptr ||
                                             std::getenv("XERENGE_D3D_TARGETS") != nullptr;
    // Same format as the swapchain, so presenting it is a straight copy.
    scene_texture_ = scene_target_enabled
                         ? device_->createTexture(plume::RenderTextureDesc::ColorTarget(
                               width, height, plume::RenderFormat::B8G8R8A8_UNORM))
                         : nullptr;
    if (scene_texture_ && PlumeSecondTargetEnabled()) {
      scene_texture1_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(
          width, height, plume::RenderFormat::B8G8R8A8_UNORM));
    }
    if (scene_texture_) {
      const plume::RenderTexture* scene_attachments[2] = {scene_texture_.get(),
                                                          scene_texture1_.get()};
      plume::RenderFramebufferDesc scene_desc;
      scene_desc.colorAttachments = scene_attachments;
      scene_desc.colorAttachmentsCount = scene_texture1_ ? 2 : 1;
      scene_desc.depthAttachment = depth_texture_.get();
      scene_framebuffer_ = device_->createFramebuffer(scene_desc);
    }
    if (scene_texture_ && !scene_framebuffer_) {
      REXLOG_ERROR("plume: could not create the {}x{} scene target; resolves stay unavailable",
                   width, height);
      scene_texture_.reset();
    }
    if (scene_texture_) {
      REXLOG_INFO("plume: rendering through a {}x{} scene target (XERENGE_SCENE_TARGET)", width,
                  height);
    }
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
    fb_desc.depthAttachment = depth_texture_.get();
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

void PlumeSwapchain::CheckProbe() {
  if (!probe_pending_ || !probe_mapped_) {
    return;
  }
  probe_pending_ = false;
  const auto* px = static_cast<const uint8_t*>(probe_mapped_);
  uint64_t sum = 0;
  for (uint32_t i = 0; i < 64 * 64; ++i) {
    sum += uint32_t(px[i * 4]) + px[i * 4 + 1] + px[i * 4 + 2];
  }
  const double mean = double(sum) / (64.0 * 64.0 * 3.0 * 255.0);
  ++probe_frames_;
  static uint32_t reported = 0;
  if (probe_frames_ > 30 && probe_average_ > 0.05 && mean < probe_average_ * 0.3 &&
      reported < 200) {
    ++reported;
    REXLOG_WARN("plume: dark frame - middle brightness {:.3f} against {:.3f}; {}", mean,
                probe_average_, probe_summary_);
  }
  probe_average_ = probe_average_ * 0.9 + mean * 0.1;
  if ((probe_frames_ % 600) == 0) {
    REXLOG_INFO("plume: frame probe - brightness {:.3f}, average {:.3f}", mean, probe_average_);
  }
}

void PlumeSwapchain::ClearAndPresent(float r, float g, float b, float a, DrawEncodeFn encode,
                                     void* encode_context, ResolveFn resolve) {
  if (!ready_ || !swap_chain_ || !command_list_ || !command_queue_ || !acquire_semaphore_ ||
      !submit_fence_) {
    return;
  }

  // The previous frame first: this frame reuses its command list, its acquire
  // semaphore and the upload buffers it read from.
  const auto previous_wait_started = std::chrono::steady_clock::now();
  if (submit_pending_) {
    command_queue_->waitForCommandFence(submit_fence_.get());
    submit_pending_ = false;
  }
  const auto previous_wait_done = std::chrono::steady_clock::now();
  CheckProbe();

  ResizeIfNeeded();
  if (!ready_ || framebuffers_.empty()) {
    return;
  }

  uint32_t image_index = 0;
  const auto acquire_started = std::chrono::steady_clock::now();
  if (!swap_chain_->acquireTexture(acquire_semaphore_.get(), &image_index)) {
    return;
  }
  const auto acquire_done = std::chrono::steady_clock::now();

  if (image_index >= framebuffers_.size() || image_index >= release_semaphores_.size()) {
    return;
  }

  plume::RenderTexture* swapchain_texture = swap_chain_->getTexture(image_index);
  const plume::RenderFramebuffer* framebuffer = framebuffers_[image_index].get();
  plume::RenderCommandSemaphore* release_semaphore = release_semaphores_[image_index].get();

  const uint32_t width = swap_chain_->getWidth();
  const uint32_t height = swap_chain_->getHeight();

  // Render into the offscreen scene target when there is one, so the frame can
  // be copied out after the pass ends. Without it, fall back to drawing
  // straight into the swapchain image - resolves are then unavailable, but the
  // picture still gets presented.
  plume::RenderTexture* const draw_target = scene_texture_ ? scene_texture_.get()
                                                           : swapchain_texture;
  const plume::RenderFramebuffer* const draw_framebuffer =
      scene_framebuffer_ ? scene_framebuffer_.get() : framebuffer;

  command_list_->begin();
  // Both attachments, not just the colour one. A depth buffer that is never
  // transitioned stays in an undefined layout, and drawing against an
  // undefined attachment is undefined behaviour - which shows up as a
  // corrupted or flickering picture rather than as an outright failure.
  plume::RenderTexture* const second_target = scene_texture_ ? scene_texture1_.get() : nullptr;
  if (depth_texture_) {
    const plume::RenderTextureBarrier attachments[3] = {
        plume::RenderTextureBarrier(draw_target, plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(depth_texture_.get(),
                                    plume::RenderTextureLayout::DEPTH_WRITE),
        plume::RenderTextureBarrier(second_target, plume::RenderTextureLayout::COLOR_WRITE)};
    command_list_->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, attachments,
                            second_target ? 3 : 2);
  } else {
    command_list_->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(draw_target, plume::RenderTextureLayout::COLOR_WRITE));
  }
  command_list_->setFramebuffer(draw_framebuffer);

  command_list_->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  command_list_->setScissors(plume::RenderRect(0, 0, width, height));
  command_list_->clearColor(0, plume::RenderColor(r, g, b, a));
  if (second_target) {
    command_list_->clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  if (depth_texture_) {
    // Far plane, matching the guest's own convention of clearing depth to 1.
    command_list_->clearDepth(true, 1.0f);
  }
  if (encode) {
    // The pass can be reopened from inside encoding, for a copy that has to
    // land between draws. Reopening restores the viewport and scissor but
    // deliberately does not clear: the draws already made must survive.
    struct PassState {
      plume::RenderCommandList* list;
      const plume::RenderFramebuffer* framebuffer;
      uint32_t width;
      uint32_t height;
    } pass_state{command_list_.get(), draw_framebuffer, width, height};
    RenderPassBreak pass{
        &pass_state,
        [](void* raw) { static_cast<PassState*>(raw)->list->setFramebuffer(nullptr); },
        [](void* raw) {
          auto* state = static_cast<PassState*>(raw);
          state->list->setFramebuffer(const_cast<plume::RenderFramebuffer*>(state->framebuffer));
          state->list->setViewports(
              plume::RenderViewport(0.0f, 0.0f, float(state->width), float(state->height)));
          state->list->setScissors(plume::RenderRect(0, 0, state->width, state->height));
        }};
    pass.second_target = second_target;
    encode(encode_context, command_list_.get(), width, height, draw_target, pass);
  }

  // Closing the pass is what makes copying out of the target legal: while it
  // is bound for writing, moving it to a copy source is not.
  command_list_->setFramebuffer(nullptr);

  if (resolve && scene_texture_) {
    resolve(encode_context, command_list_.get(), width, height, draw_target, RenderPassBreak{});
  }

  if (scene_texture_) {
    const plume::RenderTextureBarrier to_copy[2] = {
        plume::RenderTextureBarrier(draw_target, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(swapchain_texture, plume::RenderTextureLayout::COPY_DEST)};
    command_list_->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, to_copy, 2);
    if (probe_mapped_ && width >= 64 && height >= 64) {
      const plume::RenderBox box(int32_t(width / 2 - 32), int32_t(height / 2 - 32),
                                 int32_t(width / 2 + 32), int32_t(height / 2 + 32));
      command_list_->copyTextureRegion(
          plume::RenderTextureCopyLocation::PlacedFootprint(probe_buffer_.get(),
                                                            plume::RenderFormat::B8G8R8A8_UNORM,
                                                            64, 64, 1, 64, 0),
          plume::RenderTextureCopyLocation::Subresource(draw_target, 0, 0), 0, 0, 0, &box);
      probe_pending_ = true;
      probe_summary_ = g_plume_frame_summary;
    }
    command_list_->copyTexture(swapchain_texture, draw_target);
  }

  command_list_->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(swapchain_texture,
                                                      plume::RenderTextureLayout::PRESENT));
  command_list_->end();

  const plume::RenderCommandList* cmd_list = command_list_.get();
  plume::RenderCommandSemaphore* wait_semaphore = acquire_semaphore_.get();
  command_queue_->executeCommandLists(&cmd_list, 1, &wait_semaphore, 1, &release_semaphore, 1,
                                      submit_fence_.get());
  const auto submit_started = std::chrono::steady_clock::now();
  swap_chain_->present(image_index, &release_semaphore, 1);
  const auto present_done = std::chrono::steady_clock::now();
  submit_pending_ = true;
  // Waiting here, for the frame just submitted, is the default. Leaving the
  // wait to the start of the next frame lets the GPU work while the title
  // computes that frame (3.5 ms a frame in the race), but with it black frames
  // flashed up now and then and the cause is not found yet - so it is opt-in.
  static const bool async_present = std::getenv("XERENGE_ASYNC_PRESENT") != nullptr;
  if (!async_present) {
    command_queue_->waitForCommandFence(submit_fence_.get());
    submit_pending_ = false;
    CheckProbe();
  }
  const auto fence_done = std::chrono::steady_clock::now();
  {
    // Where a frame's time goes on the host side: waiting for a swap chain
    // image, recording (the encode callback), presenting, and waiting for the
    // GPU to finish - the last is what the title's thread sits in.
    static uint64_t acquire_us = 0, record_us = 0, present_us = 0, gpu_us = 0, frames = 0;
    static auto last_report = fence_done;
    auto us = [](auto a, auto b) {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    acquire_us += us(acquire_started, acquire_done);
    record_us += us(acquire_done, submit_started);
    present_us += us(submit_started, present_done);
    gpu_us += us(previous_wait_started, previous_wait_done);
    ++frames;
    if (fence_done - last_report >= std::chrono::seconds(2)) {
      REXLOG_INFO("plume: host frame (avg over {}): acquire {} us, record {} us, present {} us, "
                  "gpu wait {} us",
                  frames, acquire_us / frames, record_us / frames, present_us / frames,
                  gpu_us / frames);
      acquire_us = record_us = present_us = gpu_us = frames = 0;
      last_report = fence_done;
    }
  }
}

}  // namespace rex::plume_renderer
