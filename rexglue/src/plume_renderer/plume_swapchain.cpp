/**
 * @file        plume_renderer/plume_swapchain.cpp
 * @brief       Minimal plume swapchain: clear color + present (phase 1).
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "plume_renderer/plume_swapchain.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <plume_render_interface.h>

#include <rex/logging.h>

namespace rex::plume_renderer {

namespace {

// Three: with two and FIFO presentation the present thread waited 3.6 ms a
// frame (Ryzen 5 4500U) for an image to come back from the display, and a
// frame that missed a vblank by that much was held to the next - 30 fps.
constexpr uint32_t kBufferCount = 3;
constexpr plume::RenderFormat kSwapchainFormat = kColorTargetFormat;

// XERENGE_FPS_SHOW=1: presents per second, in the top left corner.
bool FpsShown() {
  static const bool shown = [] {
    const char* text = std::getenv("XERENGE_FPS_SHOW");
    return text != nullptr && *text != '\0' && *text != '0';
  }();
  return shown;
}

// The counter, drawn as seven-segment digits made of cleared rectangles: no
// font, no texture and no pipeline, so it costs nothing to draw and cannot
// be broken by whatever the frame itself went through. Drawn into the
// framebuffer bound when called.
void DrawFpsCounter(plume::RenderCommandList* list, uint32_t height, uint32_t fps) {
  // Segments a (top) to g (middle), clockwise from the top as usual.
  static constexpr uint8_t kDigitSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                                 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const int32_t scale = std::max<int32_t>(1, int32_t(height) / 540);
  const int32_t digit_w = 10 * scale;
  const int32_t digit_h = 18 * scale;
  const int32_t stroke = 2 * scale;
  const int32_t gap = 4 * scale;
  const int32_t margin = 6 * scale;

  char text[8];
  const int length = std::snprintf(text, sizeof(text), "%u", std::min(fps, 999u));
  const int32_t box_w = margin * 2 + length * digit_w + (length - 1) * gap;
  const int32_t box_h = margin * 2 + digit_h;
  const plume::RenderRect box(0, 0, box_w, box_h);
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f), &box, 1);

  plume::RenderRect segments[3 * 7];
  uint32_t count = 0;
  for (int i = 0; i < length; ++i) {
    const uint8_t on = kDigitSegments[text[i] - '0'];
    const int32_t x = margin + i * (digit_w + gap);
    const int32_t y = margin;
    const int32_t mid = y + digit_h / 2;
    const plume::RenderRect shapes[7] = {
        {x, y, x + digit_w, y + stroke},                                // a
        {x + digit_w - stroke, y, x + digit_w, mid},                    // b
        {x + digit_w - stroke, mid, x + digit_w, y + digit_h},          // c
        {x, y + digit_h - stroke, x + digit_w, y + digit_h},            // d
        {x, mid, x + stroke, y + digit_h},                              // e
        {x, y, x + stroke, mid},                                        // f
        {x, mid - stroke / 2, x + digit_w, mid - stroke / 2 + stroke},  // g
    };
    for (int s = 0; s < 7; ++s) {
      if (on & (1u << s)) {
        segments[count++] = shapes[s];
      }
    }
  }
  if (count != 0) {
    list->clearColor(0, plume::RenderColor(1.0f, 1.0f, 0.0f, 1.0f), segments, count);
  }
}

// XERENGE_RENDER_RESOLUTION=WIDTHxHEIGHT (run.sh --render-resolution): the size
// the frame is drawn at, scaled onto the window when presented. 0 when unset.
void RequestedRenderResolution(uint32_t* width, uint32_t* height) {
  *width = 0;
  *height = 0;
  const char* text = std::getenv("XERENGE_RENDER_RESOLUTION");
  unsigned w = 0, h = 0;
  if (text && std::sscanf(text, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 7680 &&
      h <= 4320) {
    *width = w;
    *height = h;
  }
}

// XERENGE_ASPECT_16_9 (the installer's "Keep 16:9"): the frame keeps the
// console's shape on a screen of another one, centred with black bars.
bool FixedAspect() {
  static const bool fixed = std::getenv("XERENGE_ASPECT_16_9") != nullptr;
  return fixed;
}

// The largest 16:9 rectangle inside the window, centred.
plume::RenderRect AspectRect(uint32_t window_width, uint32_t window_height) {
  uint32_t width = std::min(window_width, window_height * 16 / 9);
  uint32_t height = width * 9 / 16;
  width &= ~1u;
  height &= ~1u;
  const int32_t left = int32_t((window_width - width) / 2);
  const int32_t top = int32_t((window_height - height) / 2);
  return plume::RenderRect(left, top, left + int32_t(width), top + int32_t(height));
}

// Presents in the last whole second.
uint32_t CountFps() {
  static auto second_started = std::chrono::steady_clock::now();
  static uint32_t presents = 0;
  static uint32_t shown = 0;
  ++presents;
  const auto now = std::chrono::steady_clock::now();
  if (now - second_started >= std::chrono::seconds(1)) {
    shown = presents;
    presents = 0;
    second_started = now;
  }
  return shown;
}

// The window as plume takes it: SDL's own where plume draws through SDL's Vulkan
// surface, the Win32 handle on Windows. Not asked of SDL here on Windows:
// this DLL has its own copy of SDL, which answers null for a window the SDK's
// copy made - the swap chain then had no window at all.
plume::RenderWindow NativeRenderWindow(SDL_Window* window, void* native_window) {
#ifdef _WIN32
  (void)window;
  return static_cast<HWND>(native_window);
#elif defined(__ANDROID__)
  // plume makes the surface itself there (VK_KHR_android_surface), from the
  // ANativeWindow SDL draws into.
  (void)native_window;
  return static_cast<ANativeWindow*>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
#else
  (void)native_window;
  return window;
#endif
}

}  // namespace

PlumeSwapchain::~PlumeSwapchain() {
  Shutdown();
}

bool PlumeSwapchain::Initialize(plume::RenderDevice* device, SDL_Window* window,
                                void* native_window) {
  Shutdown();

  if (!device || !window) {
    REXLOG_ERROR("plume: swapchain init requires device and SDL window");
    return false;
  }

  device_ = device;
  window_ = window;
  native_window_ = native_window;

  command_queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!command_queue_) {
    REXLOG_ERROR("plume: failed to create command queue");
    Shutdown();
    return false;
  }

  const plume::RenderWindow render_window = NativeRenderWindow(window_, native_window_);
  swap_chain_ = command_queue_->createSwapChain(
      plume::RenderSwapChainDesc(render_window, kSwapchainFormat, kBufferCount));
  if (!swap_chain_) {
    REXLOG_ERROR("plume: failed to create swap chain");
    Shutdown();
    return false;
  }
#ifdef __ANDROID__
  android_window_ = render_window;
#endif

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
  spare_command_list_ = command_queue_->createCommandList();
  spare_submit_fence_ = device_->createCommandFence();
  spare_acquire_semaphore_ = device_->createCommandSemaphore();
  present_list_ = command_queue_->createCommandList();
  spare_present_list_ = command_queue_->createCommandList();
  scene_fence_ = device_->createCommandFence();
  spare_scene_fence_ = device_->createCommandFence();
  query_pool_ = device_->createQueryPool(2);
  spare_query_pool_ = device_->createQueryPool(2);
  if (!command_list_ || !submit_fence_ || !acquire_semaphore_ || !spare_command_list_ ||
      !spare_submit_fence_ || !spare_acquire_semaphore_) {
    REXLOG_ERROR("plume: failed to create swapchain sync objects");
    Shutdown();
    return false;
  }

  if (std::getenv("XERENGE_FRAME_PROBE") != nullptr) {
    probe_buffer_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(64 * 64 * 4));
    probe_mapped_ = probe_buffer_ ? probe_buffer_->map() : nullptr;
  }

  CreateFramebuffers();

  WindowPixelSize(&last_width_, &last_height_);

  ready_ = !framebuffers_.empty();
  if (ready_) {
    REXLOG_INFO("plume: swapchain ready ({}x{})", last_width_, last_height_);
  }
  return ready_;
}

void PlumeSwapchain::ReadGpuTime() {
  if (!query_pool_ || !query_written_) {
    return;
  }
  query_written_ = false;
  query_pool_->queryResults();
  const uint64_t* results = query_pool_->getResults();
  if (results && results[1] > results[0]) {
    gpu_busy_ns_ += results[1] - results[0];
    ++gpu_busy_frames_;
  }
}

void PlumeSwapchain::WaitForFrames() {
  if (submit_pending_ && command_queue_ && submit_fence_) {
    command_queue_->waitForCommandFence(submit_fence_.get());
  }
  submit_pending_ = false;
  if (spare_submit_pending_ && command_queue_ && spare_submit_fence_) {
    command_queue_->waitForCommandFence(spare_submit_fence_.get());
  }
  spare_submit_pending_ = false;
  if (scene_pending_ && command_queue_ && scene_fence_) {
    command_queue_->waitForCommandFence(scene_fence_.get());
  }
  scene_pending_ = false;
  if (spare_scene_pending_ && command_queue_ && spare_scene_fence_) {
    command_queue_->waitForCommandFence(spare_scene_fence_.get());
  }
  spare_scene_pending_ = false;
}

void PlumeSwapchain::Shutdown() {
  WaitForFrames();
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
  spare_acquire_semaphore_.reset();
  spare_submit_fence_.reset();
  spare_command_list_.reset();
  present_list_.reset();
  spare_present_list_.reset();
  scene_fence_.reset();
  spare_scene_fence_.reset();
  query_pool_.reset();
  spare_query_pool_.reset();
  query_written_ = spare_query_written_ = false;
  swap_chain_.reset();
  command_queue_.reset();
  device_ = nullptr;
  window_ = nullptr;
  native_window_ = nullptr;
  last_width_ = 0;
  last_height_ = 0;
}

void PlumeSwapchain::CreateFramebuffers() {
  framebuffers_.clear();
  if (!swap_chain_ || !device_) {
    return;
  }

  const uint32_t window_width = swap_chain_->getWidth();
  const uint32_t window_height = swap_chain_->getHeight();
  // The frame's own size: the window's, or the one asked for - which needs
  // the scene target, the frame being scaled from it onto the window.
  static const bool scene_wanted = std::getenv("XERENGE_SCENE_TARGET") != nullptr ||
                                   std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  uint32_t requested_width = 0;
  uint32_t requested_height = 0;
  RequestedRenderResolution(&requested_width, &requested_height);
  if (FixedAspect() && requested_width == 0) {
    // "The device's" resolution, kept 16:9: as much of the screen as that shape fills.
    const plume::RenderRect fit = AspectRect(window_width, window_height);
    requested_width = uint32_t(fit.right - fit.left);
    requested_height = uint32_t(fit.bottom - fit.top);
  }
  const bool scaled = scene_wanted && requested_width != 0 &&
                      (requested_width != window_width || requested_height != window_height);
  const uint32_t width = scaled ? requested_width : window_width;
  const uint32_t height = scaled ? requested_height : window_height;
  render_width_ = width;
  render_height_ = height;
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
                               width, height, kColorTargetFormat))
                         : nullptr;
    if (scene_texture_ && PlumeSecondTargetEnabled()) {
      scene_texture1_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(
          width, height, kColorTargetFormat));
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
      if (scaled) {
        REXLOG_INFO("plume: frames drawn at {}x{} and scaled to the {}x{} window "
                    "(XERENGE_RENDER_RESOLUTION)",
                    width, height, window_width, window_height);
      }
    } else if (scaled) {
      // No target to draw at another size into: the window's size after all.
      REXLOG_ERROR("plume: no scene target; drawing at the window's {}x{}", window_width,
                   window_height);
      render_width_ = window_width;
      render_height_ = window_height;
      depth_texture_ = device_->createTexture(plume::RenderTextureDesc::DepthTarget(
          window_width, window_height, kPlumeDepthFormat));
    }
  }
  // The window's own images take the depth target only when it is theirs in
  // size: drawn at another size, nothing but the overlay reaches them.
  plume::RenderTexture* const window_depth =
      render_width_ == window_width && render_height_ == window_height ? depth_texture_.get()
                                                                        : nullptr;

  const uint32_t texture_count = swap_chain_->getTextureCount();
  framebuffers_.reserve(texture_count);
  release_semaphores_.clear();
  release_semaphores_.reserve(texture_count);

  for (uint32_t i = 0; i < texture_count; ++i) {
    const plume::RenderTexture* color_attachment = swap_chain_->getTexture(i);
    plume::RenderFramebufferDesc fb_desc;
    fb_desc.colorAttachments = &color_attachment;
    fb_desc.colorAttachmentsCount = 1;
    fb_desc.depthAttachment = window_depth;
    framebuffers_.push_back(device_->createFramebuffer(fb_desc));
    release_semaphores_.push_back(device_->createCommandSemaphore());
  }
}

void PlumeSwapchain::WindowPixelSize(uint32_t* width, uint32_t* height) const {
#ifdef _WIN32
  RECT rect{};
  GetClientRect(static_cast<HWND>(native_window_), &rect);
  *width = uint32_t(std::max<LONG>(rect.right - rect.left, 0));
  *height = uint32_t(std::max<LONG>(rect.bottom - rect.top, 0));
#else
  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
  *width = uint32_t(std::max(pixel_width, 0));
  *height = uint32_t(std::max(pixel_height, 0));
#endif
}

void PlumeSwapchain::ResizeIfNeeded() {
  if (!window_ || !swap_chain_) {
    return;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  WindowPixelSize(&width, &height);
  if (width == last_width_ && height == last_height_) {
    return;
  }

  // The targets about to be remade may still be drawn to by a frame in flight.
  WaitForFrames();
  framebuffers_.clear();
  // A failure leaves no framebuffers, which skips the frame, and the size
  // still differs next frame, so it is tried again. Not ready_ = false: the
  // graphics system stops calling in for good once it is.
  if (!swap_chain_->resize()) {
    REXLOG_WARN("plume: swap chain resize failed");
    return;
  }

  last_width_ = width;
  last_height_ = height;
  CreateFramebuffers();
  REXLOG_INFO("plume: swapchain resized to {}x{}", last_width_, last_height_);
}

bool PlumeSwapchain::FollowAndroidWindow() {
#ifdef __ANDROID__
  if (!window_ || !command_queue_) {
    return false;
  }
  // The screen turned off or another app in front takes the window's surface
  // away, and it comes back as a new ANativeWindow. Frames presented to the
  // old one never reach the screen (black after waking the phone), so the
  // swap chain is made again on the new one.
  //
  // ready_ is left alone meanwhile: the graphics system stops calling in at
  // all once it is false, and nothing would then notice the window return.
  void* const current = NativeRenderWindow(window_, native_window_);
  if (current == android_window_) {
    return current != nullptr && swap_chain_ != nullptr;
  }
  WaitForFrames();
  framebuffers_.clear();
  release_semaphores_.clear();
  swap_chain_.reset();
  android_window_ = current;
  last_width_ = 0;
  last_height_ = 0;
  if (current == nullptr) {
    REXLOG_INFO("plume: window surface gone (in the background); not presenting");
    return false;
  }
  // A failure is tried again next frame (android_window_ cleared below).
  static uint32_t failures = 0;
  swap_chain_ = command_queue_->createSwapChain(plume::RenderSwapChainDesc(
      static_cast<plume::RenderWindow>(current), kSwapchainFormat, kBufferCount));
  if (swap_chain_ && std::getenv("XERENGE_UNLOCK_FPS") != nullptr) {
    swap_chain_->setVsyncEnabled(false);
  }
  if (!swap_chain_ || !swap_chain_->resize()) {
    if (++failures <= 5) {
      REXLOG_ERROR("plume: could not make the swap chain again on the new window surface");
    }
    swap_chain_.reset();
    android_window_ = nullptr;
    return false;
  }
  CreateFramebuffers();
  WindowPixelSize(&last_width_, &last_height_);
  if (framebuffers_.empty()) {
    if (++failures <= 5) {
      REXLOG_ERROR("plume: no framebuffers on the new window surface");
    }
    swap_chain_.reset();
    android_window_ = nullptr;
    return false;
  }
  REXLOG_INFO("plume: swap chain made again on the new window surface ({}x{})", last_width_,
              last_height_);
  return true;
#else
  return true;
#endif
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
                                     void* encode_context, ResolveFn resolve, bool hold) {
  hold = hold && scene_texture_ != nullptr;
  if (hold) {
    encode = nullptr;
    resolve = nullptr;
  }
  if (!FollowAndroidWindow()) {
    return;
  }
  if (!ready_ || !swap_chain_ || !command_list_ || !command_queue_ || !acquire_semaphore_ ||
      !submit_fence_) {
    return;
  }

  // With the wait left to the next frame, two sets take turns: this frame
  // takes the set the frame before last used, and waits only for that frame -
  // the last one may still be on the GPU while this one is written. The draw
  // context alternates the halves of its buffers the same way.
  static const bool frames_overlap = std::getenv("XERENGE_ASYNC_PRESENT") != nullptr;
  if (frames_overlap) {
    std::swap(command_list_, spare_command_list_);
    std::swap(submit_fence_, spare_submit_fence_);
    std::swap(acquire_semaphore_, spare_acquire_semaphore_);
    std::swap(submit_pending_, spare_submit_pending_);
    std::swap(query_pool_, spare_query_pool_);
    std::swap(present_list_, spare_present_list_);
    std::swap(scene_fence_, spare_scene_fence_);
    std::swap(scene_pending_, spare_scene_pending_);
    std::swap(query_written_, spare_query_written_);
  }
  // The frame that used this set first: this frame reuses its command list,
  // its acquire semaphore and the upload buffers it read from.
  const auto previous_wait_started = std::chrono::steady_clock::now();
  if (scene_pending_) {
    command_queue_->waitForCommandFence(scene_fence_.get());
    scene_pending_ = false;
  }
  if (submit_pending_) {
    command_queue_->waitForCommandFence(submit_fence_.get());
    submit_pending_ = false;
    ReadGpuTime();
  }
  const auto previous_wait_done = std::chrono::steady_clock::now();
  CheckProbe();

  ResizeIfNeeded();
  if (!ready_ || framebuffers_.empty()) {
    return;
  }

  // With a scene target the frame is recorded and handed to the GPU before a
  // swap chain image is asked for, and only the copy onto that image waits
  // for it. Asked for first, the wait for the display (7-9 ms a race frame
  // on the laptop, under vsync) came on top of recording (8-11 ms) instead of
  // alongside it, and the frame rate sat at 50 with the GPU busy 13 ms of 16.
  // XERENGE_ACQUIRE_FIRST=1 keeps the old order.
  static const bool acquire_first = std::getenv("XERENGE_ACQUIRE_FIRST") != nullptr;
  const bool split = scene_texture_ != nullptr && present_list_ != nullptr && scene_fence_ &&
                     !acquire_first;
  uint32_t image_index = 0;
  plume::RenderTexture* swapchain_texture = nullptr;
  const plume::RenderFramebuffer* framebuffer = nullptr;
  plume::RenderCommandSemaphore* release_semaphore = nullptr;
  auto acquire_started = std::chrono::steady_clock::now();
  auto acquire_done = acquire_started;
  auto acquire = [&]() {
    acquire_started = std::chrono::steady_clock::now();
    if (!swap_chain_->acquireTexture(acquire_semaphore_.get(), &image_index)) {
      return false;
    }
    acquire_done = std::chrono::steady_clock::now();
    if (image_index >= framebuffers_.size() || image_index >= release_semaphores_.size()) {
      return false;
    }
    swapchain_texture = swap_chain_->getTexture(image_index);
    framebuffer = framebuffers_[image_index].get();
    release_semaphore = release_semaphores_[image_index].get();
    return true;
  };
  if (!split && !acquire()) {
    return;
  }

  const uint32_t window_width = swap_chain_->getWidth();
  const uint32_t window_height = swap_chain_->getHeight();
  // The frame is drawn at the scene target's size, which differs from the
  // window's under XERENGE_RENDER_RESOLUTION.
  const uint32_t width = scene_texture_ ? render_width_ : window_width;
  const uint32_t height = scene_texture_ ? render_height_ : window_height;

  // Render into the offscreen scene target when there is one, so the frame can
  // be copied out after the pass ends. Without it, fall back to drawing
  // straight into the swapchain image - resolves are then unavailable, but the
  // picture still gets presented.
  plume::RenderTexture* const draw_target = scene_texture_ ? scene_texture_.get()
                                                           : swapchain_texture;
  const plume::RenderFramebuffer* const draw_framebuffer =
      scene_framebuffer_ ? scene_framebuffer_.get() : framebuffer;

  const auto record_started = std::chrono::steady_clock::now();
  command_list_->begin();
  if (query_pool_) {
    command_list_->resetQueryPool(query_pool_.get(), 0, 2);
    command_list_->writeTimestamp(query_pool_.get(), 0);
  }
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
  // A held frame keeps what the scene target holds: the last picture.
  if (!hold) {
    command_list_->clearColor(0, plume::RenderColor(r, g, b, a));
    if (second_target) {
      command_list_->clearColor(1, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
    if (depth_texture_) {
      // Far plane, matching the guest's own convention of clearing depth to 1.
      command_list_->clearDepth(true, 1.0f);
    }
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

  const uint32_t fps = FpsShown() ? CountFps() : 0;
  if (FpsShown() && !scene_texture_) {
    // Drawn straight into the swap chain image: the counter goes on last.
    command_list_->setFramebuffer(draw_framebuffer);
    DrawFpsCounter(command_list_.get(), height, fps);
  }
  // Closing the pass is what makes copying out of the target legal: while it
  // is bound for writing, moving it to a copy source is not.
  command_list_->setFramebuffer(nullptr);

  if (resolve && scene_texture_) {
    resolve(encode_context, command_list_.get(), width, height, draw_target, RenderPassBreak{});
  }

  // Where the copy onto the swap chain image is recorded: the frame's own
  // list, or - split - the small list submitted after the image is acquired.
  plume::RenderCommandList* out = command_list_.get();
  if (split) {
    command_list_->barriers(
        plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(draw_target, plume::RenderTextureLayout::COPY_SOURCE));
    command_list_->end();
    const plume::RenderCommandList* scene_list = command_list_.get();
    command_queue_->executeCommandLists(&scene_list, 1, nullptr, 0, nullptr, 0, scene_fence_.get());
    scene_pending_ = true;
    if (!acquire()) {
      return;
    }
    out = present_list_.get();
    out->begin();
  }
  const auto record_done = std::chrono::steady_clock::now();

  if (scene_texture_) {
    const plume::RenderTextureBarrier to_copy[2] = {
        plume::RenderTextureBarrier(draw_target, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(swapchain_texture, plume::RenderTextureLayout::COPY_DEST)};
    out->barriers(plume::RenderBarrierStage::COPY, nullptr, 0, to_copy, 2);
    if (probe_mapped_ && width >= 64 && height >= 64) {
      const plume::RenderBox box(int32_t(width / 2 - 32), int32_t(height / 2 - 32),
                                 int32_t(width / 2 + 32), int32_t(height / 2 + 32));
      out->copyTextureRegion(
          plume::RenderTextureCopyLocation::PlacedFootprint(probe_buffer_.get(),
                                                            kColorTargetFormat,
                                                            64, 64, 1, 64, 0),
          plume::RenderTextureCopyLocation::Subresource(draw_target, 0, 0), 0, 0, 0, &box);
      probe_pending_ = true;
      probe_summary_ = g_plume_frame_summary;
    }
    // At another size (XERENGE_RENDER_RESOLUTION) the frame is scaled onto
    // the window, filtered; at the window's own size it is a straight copy.
    if (width != window_width || height != window_height) {
      const plume::RenderRect letterbox = AspectRect(window_width, window_height);
      if (!out->blitTexture(swapchain_texture, draw_target, true, FixedAspect() ? &letterbox : nullptr)) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          REXLOG_ERROR("plume: this backend cannot scale the frame onto the window");
        }
      }
    } else {
      out->copyTexture(swapchain_texture, draw_target);
    }
    // On the image shown, after the copy: drawn into the target, the counter
    // would go wherever the title copies that target to.
    if (FpsShown()) {
      out->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(swapchain_texture,
                                                plume::RenderTextureLayout::COLOR_WRITE));
      out->setFramebuffer(framebuffer);
      DrawFpsCounter(out, window_height, fps);
      out->setFramebuffer(nullptr);
    }
  }

  out->barriers(plume::RenderBarrierStage::NONE,
                plume::RenderTextureBarrier(swapchain_texture,
                                            plume::RenderTextureLayout::PRESENT));
  if (query_pool_) {
    out->writeTimestamp(query_pool_.get(), 1);
    query_written_ = true;
  }
  out->end();

  const plume::RenderCommandList* cmd_list = out;
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
    if (scene_pending_) {
      command_queue_->waitForCommandFence(scene_fence_.get());
      scene_pending_ = false;
    }
    ReadGpuTime();
    CheckProbe();
  }
  const auto fence_done = std::chrono::steady_clock::now();
  {
    // Where a frame's time goes on the host side: waiting for a swap chain
    // image, recording (the encode callback), presenting, and waiting for the
    // GPU to finish - the last is what the title's thread sits in.
    static uint64_t acquire_us = 0, record_us = 0, present_us = 0, gpu_us = 0, fence_us = 0,
                    frames = 0;
    static auto last_report = fence_done;
    auto us = [](auto a, auto b) {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    acquire_us += us(acquire_started, acquire_done);
    record_us += us(record_started, record_done);
    present_us += us(submit_started, present_done);
    gpu_us += us(previous_wait_started, previous_wait_done);
    // The wait for this frame's own GPU work, done right after presenting it
    // unless XERENGE_ASYNC_PRESENT - the part of a frame the GPU takes.
    fence_us += us(present_done, fence_done);
    ++frames;
    if (fence_done - last_report >= std::chrono::seconds(2)) {
      REXLOG_INFO("plume: host frame (avg over {}): acquire {} us, record {} us, present {} us, "
                  "previous frame {} us, this frame's GPU {} us; GPU busy {} us a frame",
                  frames, acquire_us / frames, record_us / frames, present_us / frames,
                  gpu_us / frames, fence_us / frames,
                  gpu_busy_frames_ ? gpu_busy_ns_ / gpu_busy_frames_ / 1000 : 0);
      gpu_busy_ns_ = gpu_busy_frames_ = 0;
      acquire_us = record_us = present_us = gpu_us = fence_us = frames = 0;
      last_report = fence_done;
    }
  }
}

}  // namespace rex::plume_renderer
