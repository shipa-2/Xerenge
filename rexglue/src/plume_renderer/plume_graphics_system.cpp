/**
 * @file        plume_renderer/plume_graphics_system.cpp
 * @brief       Plume GPU backend implementation (phase 1).
 */
#include <set>
#include "plume_renderer/plume_graphics_system.h"
#include "diagnostics.h"

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
#include <rex/ui/renderdoc_api.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "frame_clock_provider.h"
#include "plume_renderer/plume_draw.h"
#include "plume_renderer/plume_interp.h"
#include "plume_renderer/plume_parallel.h"
#include "plume_renderer/plume_shader_cache.h"
#include "plume_renderer/plume_swapchain.h"
#include "shader_cache.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#endif

namespace rex::plume_renderer {

namespace {
// Encoding time of the present being made on this thread, for its breakdown.
thread_local uint64_t g_present_encode_us = 0;

#if defined(__ANDROID__)
// The title's thread (the one that calls VdSwap), for the frame hint below.
std::atomic<int> g_title_tid{0};

// Android's performance hints (ADPF, Android 13 on): a session naming the
// threads a frame is made on and how long a frame may take, told every frame
// how long the last one did. Without it the scheduler kept the title's and the
// present thread on the Pixel's middle cores at their own pace - the frame
// took 55 ms with neither thread busy half the time. Looked up at run time,
// so the APK still starts on Android 10; XERENGE_NO_PERF_HINT=1 leaves it off.
class FrameHint {
 public:
  void Report(int present_tid, int64_t took_ns) {
    if (failed_) {
      return;
    }
    if (!session_ && !Start(present_tid)) {
      return;
    }
    report_(session_, took_ns);
  }

 private:
  using GetManager = void* (*)();
  using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportDuration = int (*)(void*, int64_t);

  bool Start(int present_tid) {
    const int title_tid = g_title_tid.load(std::memory_order_relaxed);
    if (title_tid == 0) {
      return false;  // not until the title has swapped once
    }
    failed_ = true;
    if (std::getenv("XERENGE_NO_PERF_HINT") != nullptr) {
      return false;
    }
    void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    const auto get_manager =
        android ? reinterpret_cast<GetManager>(dlsym(android, "APerformanceHint_getManager")) : nullptr;
    const auto create = android ? reinterpret_cast<CreateSession>(
                                      dlsym(android, "APerformanceHint_createSession"))
                                : nullptr;
    report_ = android ? reinterpret_cast<ReportDuration>(
                            dlsym(android, "APerformanceHint_reportActualWorkDuration"))
                      : nullptr;
    void* manager = get_manager ? get_manager() : nullptr;
    if (!manager || !create || !report_) {
      REXLOG_INFO("plume: no performance hints on this system (Android 13 has them)");
      return false;
    }
    std::vector<int32_t> tids = {present_tid, title_tid};
    for (const int worker : WorkerPool::Get().thread_ids()) {
      tids.push_back(worker);
    }
    constexpr int64_t kFrameNs = 16666667;  // the title's 60 Hz
    session_ = create(manager, tids.data(), tids.size(), kFrameNs);
    if (!session_) {
      REXLOG_WARN("plume: performance hint session refused for {} threads", tids.size());
      return false;
    }
    failed_ = false;
    REXLOG_INFO("plume: performance hints on for {} threads (present, title, {} workers)",
                tids.size(), tids.size() - 2);
    return true;
  }

  void* session_ = nullptr;
  ReportDuration report_ = nullptr;
  bool failed_ = false;
};
#endif
// Per-present accounting for the overlay ring. All four are read and written
// only under snapshot_mutex_.
// A short history of what the command processor last did, so a stall can be
// described by the state the guest left behind rather than guessed at. Written
// under ring_mutex_ / resolve_mutex_ by the paths that record into it.
struct RecentGpuEvent {
  const char* kind = "";
  uint32_t a = 0;
  uint32_t b = 0;
};
constexpr size_t kRecentGpuEvents = 24;
RecentGpuEvent g_recent_events[kRecentGpuEvents];
size_t g_recent_next = 0;
std::mutex g_recent_mutex;

// Addresses the title had fences written to, so their current contents can be
// shown when it stops: the value it is waiting on is one of these.
uint32_t g_fence_addresses[4] = {};
size_t g_fence_count = 0;

void NoteFenceAddress(uint32_t address) {
  const uint32_t aligned = address & ~uint32_t(0x3);
  for (size_t i = 0; i < g_fence_count; ++i) {
    if (g_fence_addresses[i] == aligned) {
      return;
    }
  }
  if (g_fence_count < std::size(g_fence_addresses)) {
    g_fence_addresses[g_fence_count++] = aligned;
  }
}

void NoteGpuEvent(const char* kind, uint32_t a, uint32_t b) {
  std::lock_guard lock(g_recent_mutex);
  g_recent_events[g_recent_next] = RecentGpuEvent{kind, a, b};
  g_recent_next = (g_recent_next + 1) % kRecentGpuEvents;
}

void DumpRecentGpuEvents() {
  std::lock_guard lock(g_recent_mutex);
  REXLOG_WARN("plume: what the command processor last did, oldest first:");
  for (size_t i = 0; i < kRecentGpuEvents; ++i) {
    const RecentGpuEvent& e = g_recent_events[(g_recent_next + i) % kRecentGpuEvents];
    if (e.kind[0] != '\0') {
      REXLOG_WARN("plume:   {} {:08X} {:08X}", e.kind, e.a, e.b);
    }
  }
}

uint32_t g_ring_pushed_since_present = 0;
uint32_t g_ring_rejected_since_present = 0;
uint32_t g_ring_rejected_no_vs = 0;
uint32_t g_ring_rejected_no_ps = 0;
uint32_t g_ring_rejected_few_indices = 0;
uint32_t g_ring_overwritten_since_present = 0;
uint32_t g_video_classified_since_present = 0;

bool VideoFrameWeakerThan(const GuestDrawSnapshot& next, const GuestDrawSnapshot& current) {
  if (!current.has_video_frame()) {
    return false;
  }
  if (next.video_luma_range + 8 < current.video_luma_range && next.video_luma_mean < 28) {
    return true;
  }
  return next.video_luma_mean + 28 < current.video_luma_mean && next.video_luma_mean < 22;
}

#if defined(__ANDROID__)
// A Vulkan driver the installer put in the app's own files (Mesa Turnip, for
// Adreno GPUs whose stock driver lacks what the shaders need: Vulkan 1.2,
// 64-bit integers, descriptor indexing). BurnoutActivity passes where it is:
//   XERENGE_VULKAN_DRIVER   the driver .so, in internal storage (dlopen refuses
//                           libraries on shared storage)
//   XERENGE_NATIVE_LIB_DIR  the app's nativeLibraryDir, where libadrenotools'
//                           hook libraries are
// libadrenotools opens it in place of the system driver, and plume takes its
// vkGetInstanceProcAddr. Without it, or should it fail, the system driver.
void UseAndroidCustomVulkanDriver() {
  const char* driver = std::getenv("XERENGE_VULKAN_DRIVER");
  const char* hooks = std::getenv("XERENGE_NATIVE_LIB_DIR");
  if (!driver || !*driver || !hooks || !*hooks) {
    REXLOG_INFO("plume: using the system Vulkan driver");
    return;
  }
  const std::string path(driver);
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return;
  }
  const std::string directory = path.substr(0, slash + 1);
  const std::string name = path.substr(slash + 1);
  const std::string hook_dir = std::string(hooks) + "/";
  void* vulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                            hook_dir.c_str(), directory.c_str(), name.c_str(),
                                            nullptr, nullptr);
  if (!vulkan) {
    REXLOG_ERROR("plume: the custom Vulkan driver {} did not load; using the system one", path);
    return;
  }
  auto get_instance_proc_addr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan, "vkGetInstanceProcAddr"));
  if (!get_instance_proc_addr) {
    REXLOG_ERROR("plume: {} has no vkGetInstanceProcAddr; using the system driver", path);
    return;
  }
  plume::SetVulkanGetInstanceProcAddr(get_instance_proc_addr);
  REXLOG_INFO("plume: using the custom Vulkan driver {}", path);
}
#endif
}  // namespace

PlumeGraphicsSystem::PlumeGraphicsSystem() {
  draw_ring_.resize(kDrawRingSize);
}

PlumeGraphicsSystem::~PlumeGraphicsSystem() {
  Shutdown();
}

bool PlumeGraphicsSystem::InitializePlumeDevice() {
  if (render_device_) {
    return true;
  }

  // Load RenderDoc before any Vulkan instance exists, as the xenos backend
  // does through the SDK's own instance. Loading the library in-process is
  // what switches its capture layer on for instances created afterwards; this
  // backend builds its instance itself and never did it, so there was no
  // overlay and F12 captured nothing. The library is only present where
  // RenderDoc is installed, so elsewhere this does nothing.
  if (!renderdoc_api_) {
    renderdoc_api_ = rex::ui::RenderDocAPI::CreateIfConnected();
    if (renderdoc_api_ && renderdoc_api_->api_1_0_0()) {
      // This template overrides renderdoccmd's --capture-file, so run.sh
      // --capture names its per-run directory here instead.
      const char* dir = std::getenv("XERENGE_CAPTURE_DIR");
      const std::string capture_dir = dir && *dir ? dir : "logs/captures";
      renderdoc_api_->api_1_0_0()->SetLogFilePathTemplate((capture_dir + "/plume").c_str());
      REXLOG_INFO("plume: RenderDoc loaded - F12 captures to {}/", capture_dir);
    }
  }

#if defined(__ANDROID__)
  UseAndroidCustomVulkanDriver();
#endif
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
  if (!swapchain_->Initialize(render_device_.get(), sdl_window,
                              window_->GetNativeWindowHandle())) {
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

// The title's own shader object, associated with the translated shader its
// microcode hashes to. Driving the renderer from the Direct3D calls means a
// draw names its shaders by object address and nothing else, so without this
// the draw cannot be built at all.
void PlumeGraphicsSystem::RegisterGuestShaderObject(uint32_t object_guest,
                                                    const void* shader_container,
                                                    bool pixel_shader) {
  // The title builds these objects itself, so nothing here can hash them into
  // a translated shader. But the order it works in gives the answer for free:
  // it binds the shader through Direct3D and only then does its Direct3D layer
  // put the matching IM_LOAD into the command ring. So remember which object
  // was bound last, and let the load - which does carry the microcode - say
  // what it was.
  (void)shader_container;
  if (!object_guest) {
    return;
  }
  // See BindLoadedShader: with the Direct3D path on, only the title's own
  // shader loads name shader objects.
  static const bool d3d_loads_authoritative = std::getenv("XERENGE_D3D_DRAWS") != nullptr;
  if (d3d_loads_authoritative) {
    return;
  }
  // The object names its own microcode: a vertex one keeps the pointer at
  // +0x28, a pixel one at +0x0C. That identifies the shader outright, where
  // pairing bindings with loads in order only works while the title does both
  // in the same order - and when it does not, scene vertices get unpacked with
  // the interface shader's layout, which is what the absurd coordinates were.
  if (ResolveShaderObject(object_guest, pixel_shader)) {
    return;
  }

  std::lock_guard lock(guest_shader_mutex_);
  auto& queue = pending_shader_objects_[pixel_shader ? 1 : 0];
  if (std::find(queue.begin(), queue.end(), object_guest) == queue.end()) {
    queue.push_back(object_guest);
    // A bound shader is always followed by its load, so the queue drains; the
    // bound keeps a lost load from growing it without end.
    while (queue.size() > 64) {
      queue.pop_front();
    }
  }
}

// What shader an object stands for, asked of the object itself. A vertex one
// keeps its microcode pointer at +0x28, a pixel one at +0x0C; the loads have
// recorded what lives at those addresses and how long it is, so re-hashing
// that many bytes says which load the memory still holds.
//
// This is worth asking again at draw time. A bind happens before the matching
// load, so the first bind of a shader can only ever miss here - and pairing
// binds with loads in order then hands the load to whichever object had been
// waiting longest, which in a scene is one left over from the menu. By the
// time anything is drawn the load has certainly happened, and the object can
// answer for itself.
bool PlumeGraphicsSystem::ResolveShaderObject(uint32_t object_guest, bool pixel_shader) {
  if (!object_guest || !memory_) {
    return false;
  }
  const auto* words = memory_->TranslateVirtual<const rex::be_u32*>(
      object_guest + (pixel_shader ? 0x0Cu : 0x28u));
  if (!words) {
    return false;
  }
  const uint32_t microcode = static_cast<uint32_t>(*words);
  if (microcode != 0) {
    const uint32_t masked = microcode & 0x1FFFFFFFu;
    const uint32_t physical =
        (microcode & 0xF0000000u) == 0xE0000000u ? masked + 0x1000u : masked;
    std::lock_guard<std::mutex> map_lock(guest_shader_mutex_);
    for (const uint32_t at : {physical, masked}) {
      const auto it = microcode_at_address_.find(at);
      if (it == microcode_at_address_.end()) {
        continue;
      }
      // Ask the memory which of the loads seen here it actually holds now.
      // Only lengths that a real load carried are ever hashed, so this cannot
      // read past the microcode the way guessing at a length would.
      for (const MicrocodeLoad& load : it->second) {
        if (HashGuestPhysical(at, load.byte_count) != load.hash) {
          continue;
        }
        // The load names microcode; a draw needs the translated shader that
        // microcode stands for. Binding by load taught the objects the latter,
        // so this path has to agree - handing back a microcode hash here left
        // the draw looking for a pipeline under a name nothing is filed under.
        const ShaderMicrocodeEntry* micro =
            PlumeShaderCache::Instance().FindByMicrocode(load.hash);
        const uint64_t resolved = micro && micro->shaderHash ? micro->shaderHash : load.hash;
        guest_shaders_[object_guest] = GuestShaderSlot{resolved, pixel_shader};
        const uint32_t n = shader_object_log_count_.fetch_add(1);
        if (n < 8) {
          REXLOG_INFO("plume: shader object {:08X} names {} {:016X} at {:08X} ({} bytes)",
                      object_guest, pixel_shader ? "PS" : "VS", resolved, microcode,
                      load.byte_count);
        }
        return true;
      }
    }

  }
  return false;
}

void PlumeGraphicsSystem::NoteGuestDraw(uint32_t primitive_type, uint32_t index_count,
                                        const GuestDrawBuffers& buffers) {
  static const bool enabled = std::getenv("XERENGE_D3D_DRAWS") != nullptr;
  if (!enabled || index_count == 0) {
    return;
  }
  // This runs on the title's own thread, once per draw. Entering a scene the
  // title slows to about two frames a second while issuing thousands of these,
  // so what it costs has to be known rather than assumed.
  // One draw in sixteen is timed and counted sixteen times: reading the clock
  // on every one was itself an eighth of the title's thread on the Pixel.
  static thread_local uint32_t note_tick = 0;
  const bool timed = xerenge::Diagnostics() && (++note_tick & 15u) == 0;
  const auto note_started =
      timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  struct NoteTimer {
    std::chrono::steady_clock::time_point started;
    bool timed;
    ~NoteTimer() {
      static std::atomic<uint64_t> total_ns{0};
      static std::atomic<uint64_t> calls{0};
      static std::atomic<uint64_t> last_ms{0};
      const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
      if (!timed) {
        return;
      }
      const auto finished = std::chrono::steady_clock::now();
      const uint64_t took = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
      total_ns.fetch_add(took * 16, std::memory_order_relaxed);
      const uint64_t now = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(finished.time_since_epoch())
              .count());
      uint64_t was = last_ms.load(std::memory_order_relaxed);
      if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
        const uint64_t total = total_ns.load(std::memory_order_relaxed);
        REXLOG_INFO("plume: Direct3D draws noted: {}, {} us each, {} ms of the title's time", n,
                    total / (n ? n : 1) / 1000, total / 1000000);
      }
    }
  } note_timer{note_started, timed};
  // Draw it here. Waiting for the ring's own draw packet to arrive and joining
  // the two halves there worked for the menu but not for the scene: inside a
  // tiled pass the title issues hundreds of these and the ring carries none of
  // them, so there is nothing to join with. Everything needed is already in
  // hand - the buffers came with the call, and the shaders resolve through the
  // objects the title bound, which the microcode loads have since identified.
  // Only indexed draws, and only ones that named a vertex buffer. The title
  // draws its interface through BeginVertices, whose vertices are written
  // inline rather than taken from a bound stream - reading stream zero for
  // those gives whatever the last mesh left there, which is where the stripes
  // came from. Those draws already reach the screen through the command ring;
  // it is the scene's indexed draws that do not.
  // Non-indexed draws are welcome now that each kind hands over the vertices it
  // really draws: DrawVertices its stream, BeginVertices the memory it gave
  // the title to write into.
  if (buffers.vertex_buffer == 0 || buffers.vertex_stride == 0) {
    return;
  }

  // The shaders come from the title's own loads, recorded by NoteShaderLoad
  // during the draw call this follows. Each slot also records which stage it
  // was loaded as, and a pair whose stages are not vertex and pixel is refused
  // here: handed to the driver, a vertex shader in the fragment stage hung its
  // compiler and with it the whole frame.
  uint64_t vs_hash = 0;
  uint64_t ps_hash = 0;
  {
    std::lock_guard lock(guest_shader_mutex_);
    if (auto it = guest_shaders_.find(buffers.vertex_shader_object);
        it != guest_shaders_.end() && !it->second.pixel_shader) {
      vs_hash = it->second.hash;
    }
    if (auto it = guest_shaders_.find(buffers.pixel_shader_object);
        it != guest_shaders_.end() && it->second.pixel_shader) {
      ps_hash = it->second.hash;
    }
  }
  if (vs_hash == 0 || ps_hash == 0) {
    const uint32_t n = d3d_unresolved_log_count_.fetch_add(1);
    if (n < 8 || (n % 20000) == 0) {
      REXLOG_INFO("plume: Direct3D draw with unknown shaders vs={:08X} ps={:08X}",
                  buffers.vertex_shader_object, buffers.pixel_shader_object);
    }
    return;
  }
  pending_d3d_buffers_ = buffers;
  pending_d3d_shaders_[0] = vs_hash;
  pending_d3d_shaders_[1] = ps_hash;
  PublishDrawSnapshot(primitive_type, 2, index_count);
  pending_d3d_buffers_ = GuestDrawBuffers{};
  pending_d3d_shaders_[0] = 0;
  pending_d3d_shaders_[1] = 0;
  // Report the big draws and the small ones separately. One shared cap let the
  // interface's own three-index draws use up every slot, so the scene draws -
  // the ones this whole path exists for - never appeared at all.
  const bool substantial = index_count >= 64;
  auto& counter = substantial ? d3d_big_draw_log_count_ : d3d_draw_log_count_;
  const uint32_t n = counter.fetch_add(1);
  if (n < 8 || (n % 20000) == 0) {
    REXLOG_INFO("plume: draw from Direct3D prim={} indices={} vb={:08X}+{} ib={:08X} "
                "vs={:016X} ps={:016X}",
                primitive_type, index_count, buffers.vertex_buffer, buffers.vertex_stride,
                buffers.index_buffer, active_vs_hash_, active_ps_hash_);
  }
}

void PlumeGraphicsSystem::BindGuestD3DDevice(uint32_t device_guest) {
  if (device_guest >= 0x10000000 && device_guest < 0x80000000 && (device_guest & 0xF) == 0) {
    d3d_device_guest_.store(device_guest, std::memory_order_relaxed);
    NoteD3DDeviceForTrap(device_guest);
  }
}

namespace {

constexpr uint32_t kD3DRingOffset = 0x28;
// Texture and vertex fetch constants as Direct3D prepares them for the GPU:
// physical addresses, window offsets applied. 0x80 on from the reference
// implementation's 0x400, like every other block in this build's device -
// SetCurrentVertexShader copies into device + 0x480 in the beta's code.
constexpr uint32_t kD3DFetchOffset = 0x480;
// Where this build's device keeps its shader constants, read off the code that
// writes them rather than borrowed: the beta's
// CB4ShaderManagerXenon::SetVertexShaderConstant stores register N at
// device + (N + 0x78) * 16, its pixel twin at (N + 0x178) * 16, and
// D3DDevice_SetVertexShaderConstantB writes its words at (0x9E0 + k) * 4.
//
// The reference implementation asserts 0x700 / 0x1700 / 0x2700 - right for
// its own title, 0x80 short for this one. Read from there, the scene's object
// matrix, which its shaders take from c0..c3, came back as zeros: every vertex
// collapsed to the origin and the screen stayed black. The same matrix showed
// up eight registers later, at "c8", which is what gave the offset away.
constexpr uint32_t kD3DVsFloatOffset = 0x780;
constexpr uint32_t kD3DPsFloatOffset = 0x1780;
constexpr uint32_t kD3DVsBoolOffset = 0x2780;
constexpr uint32_t kD3DPsBoolOffset = 0x2790;
constexpr uint32_t kD3DDeviceMinBytes = 0x2800;
// RB_DEPTHCONTROL's shadow; RB_BLENDCONTROL0 follows it.
constexpr uint32_t kD3DRenderStateOffset = 0x2D74;
constexpr uint32_t kD3DColorMaskOffset = 0x2D1C;

bool RangeReadable(memory::Memory* memory, uint32_t guest_va, uint32_t bytes) {
  // The upper bound used to stop at 0x80000000, which put the window the GPU
  // reads through out of reach - and the scene keeps its textures there, so
  // every one of them was silently dropped. The real guard is below: the heap
  // lookup and the access query, which is what keeps an unchecked translation
  // from taking the process down.
  if (!memory || guest_va < 0x10000000 || bytes == 0) {
    return false;
  }
  const uint32_t last = guest_va + bytes - 1;
  if (last < guest_va) {
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

// The Direct3D device's block, once found readable. The device lives as long
// as the title, and every draw reads it several times over: asking the heap
// each time - a recursive mutex shared with allocation and the write-watch
// fault handler, twenty times a draw, a thousand draws a frame - was most of
// what a draw cost the title's thread.
std::atomic<uint32_t> g_readable_device{0};

bool DeviceRangeReadable(memory::Memory* memory, uint32_t device_guest) {
  if (device_guest != 0 && device_guest == g_readable_device.load(std::memory_order_relaxed)) {
    return true;
  }
  if (!RangeReadable(memory, device_guest, kD3DDeviceMinBytes)) {
    return false;
  }
  g_readable_device.store(device_guest, std::memory_order_relaxed);
  return true;
}

// CopyBeDwords for a range inside the device's block, which the caller has
// checked with DeviceRangeReadable.
void CopyDeviceDwords(memory::Memory* memory, uint32_t* dst, uint32_t device_guest,
                      uint32_t offset, uint32_t count) {
  if (offset + count * 4 > kD3DDeviceMinBytes) {
    CopyBeDwords(memory, dst, device_guest + offset, count);
    return;
  }
  const uint8_t* src = memory->TranslateVirtual<const uint8_t*>(device_guest + offset);
  for (uint32_t i = 0; i < count; ++i) {
    dst[i] = rex::memory::load_and_swap<uint32_t>(src + i * 4);
  }
}

}  // namespace

bool PlumeGraphicsSystem::DeviceLooksValid(uint32_t device_guest) const {
  if (!memory_ || !DeviceRangeReadable(memory_, device_guest)) {
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
  CopyDeviceDwords(memory_, &gpu_registers_[0x4000], device_guest, kD3DVsFloatOffset, 1024);
  CopyDeviceDwords(memory_, &gpu_registers_[0x4400], device_guest, kD3DPsFloatOffset, 1024);
  CopyDeviceDwords(memory_, &gpu_registers_[0x4800], device_guest, kD3DFetchOffset, 192);
  CopyDeviceDwords(memory_, &gpu_registers_[0x4900], device_guest, kD3DVsBoolOffset, 4);
  CopyDeviceDwords(memory_, &gpu_registers_[0x4904], device_guest, kD3DPsBoolOffset, 4);

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

// The worker owns the decode. One request outstanding at a time: if a frame
// arrives while the previous is still being decoded it simply replaces it -
// showing the newest frame late beats queueing frames nobody will see.
void PlumeGraphicsSystem::RequestVideoCapture(const GuestDrawSnapshot& snap) {
  // The planes are copied here, on the thread that saw the draw, while they
  // still hold this frame. Read later on the worker, the decoder was already
  // writing the next one into them, and the menu video came out torn.
  GuestDrawSnapshot request = snap;
  CopyGuestVideoPlanes(&request, memory_);
  {
    std::lock_guard lock(video_request_mutex_);
    video_request_ = std::move(request);
    video_request_pending_ = true;
  }
  video_request_cv_.notify_one();
  StartVideoWorker();
}

void PlumeGraphicsSystem::StartVideoWorker() {
  if (video_worker_running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  video_worker_ = std::thread([this] {
    for (;;) {
      GuestDrawSnapshot request;
      {
        std::unique_lock lock(video_request_mutex_);
        video_request_cv_.wait(lock, [this] {
          return video_request_pending_ || !video_worker_running_.load(std::memory_order_acquire);
        });
        if (!video_worker_running_.load(std::memory_order_acquire)) {
          return;
        }
        request = std::move(video_request_);
        video_request_pending_ = false;
      }
      if (!CaptureGuestVideoFrame(&request, memory_)) {
        continue;
      }
      video_captures_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard lock(snapshot_mutex_);
      last_video_key_.store(request.video_key, std::memory_order_relaxed);
      pending_video_ = std::move(request);
    }
  });
}

void PlumeGraphicsSystem::StopVideoWorker() {
  if (!video_worker_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  video_request_cv_.notify_all();
  if (video_worker_.joinable()) {
    video_worker_.join();
  }
}

void PlumeGraphicsSystem::StartPresentWorker() {
  if (present_worker_running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  present_worker_ = std::thread([this] {
    for (;;) {
      uint32_t w = 0, h = 0;
      {
        std::unique_lock lock(present_request_mutex_);
        present_request_cv_.wait(lock, [this] {
          return present_request_pending_ || !present_worker_running_.load(std::memory_order_acquire);
        });
        if (!present_worker_running_.load(std::memory_order_acquire)) {
          return;
        }
        w = present_request_width_;
        h = present_request_height_;
        present_request_pending_ = false;
      }
#if defined(__ANDROID__)
      static FrameHint frame_hint;
      const auto work_started = std::chrono::steady_clock::now();
#endif
      PresentClearColorOnUiThread(w, h);
#if defined(__ANDROID__)
      frame_hint.Report(int(gettid()),
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - work_started)
                            .count());
#endif
      {
        std::lock_guard lock(present_flight_mutex_);
        present_in_flight_ = false;
      }
      present_flight_cv_.notify_one();
    }
  });
}

void PlumeGraphicsSystem::StopPresentWorker() {
  if (!present_worker_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  present_request_cv_.notify_all();
  if (present_worker_.joinable()) {
    present_worker_.join();
  }
}

void PlumeGraphicsSystem::PublishDrawSnapshot(uint32_t prim_type, uint32_t source_select,
                                              uint32_t num_indices) {
  // Both halves of this are per-draw and neither is cheap: the pull copies
  // 2240 constants out of guest memory with a byte swap on every one, and the
  // snapshot then carries its own 9 KB copy of them. A scene frame issues
  // ~175 draws, so measure the two separately before assuming which dominates.
  // Timed one draw in sixteen and counted sixteen times, as NoteGuestDraw.
  static thread_local uint32_t snapshot_tick = 0;
  const bool timed = xerenge::Diagnostics() && (++snapshot_tick & 15u) == 0;
  const auto clock_now = [timed] {
    return timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  };
  const auto snapshot_started = clock_now();
  const uint32_t device = FindD3DDevice();
  if (device) {
    PullDeviceConstants(device);
  }
  const auto pull_done = clock_now();
  struct PhaseTimer {
    std::chrono::steady_clock::time_point started, pull_done;
    bool timed;
    std::atomic<uint64_t>* pull_us;
    std::atomic<uint64_t>* copy_us;
    std::atomic<uint64_t>* draws;
    ~PhaseTimer() {
      draws->fetch_add(1, std::memory_order_relaxed);
      if (!timed) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      pull_us->fetch_add(
          16 * std::chrono::duration_cast<std::chrono::nanoseconds>(pull_done - started).count(),
          std::memory_order_relaxed);
      copy_us->fetch_add(
          16 * std::chrono::duration_cast<std::chrono::nanoseconds>(now - pull_done).count(),
          std::memory_order_relaxed);
    }
  } phase_timer{snapshot_started, pull_done, timed, &snapshot_pull_ns_, &snapshot_copy_ns_,
                &snapshot_draws_};

  GuestDrawSnapshot snap;
  snap.vs_hash = active_vs_hash_;
  snap.ps_hash = active_ps_hash_;
  // The buffers of a draw that came straight from Direct3D, if this is one.
  const GuestDrawBuffers buffers = pending_d3d_buffers_;
  if (pending_d3d_shaders_[0] != 0) {
    snap.vs_hash = pending_d3d_shaders_[0];
    snap.ps_hash = pending_d3d_shaders_[1];
  }
  snap.d3d_vertex_buffer = buffers.vertex_buffer;
  snap.interface_draw = buffers.from_interface;
  snap.interface_left = buffers.interface_left;
  snap.interface_object = buffers.interface_object;
  snap.d3d_vertex_stride = buffers.vertex_stride;
  snap.d3d_index_buffer = buffers.index_buffer;
  snap.d3d_base_vertex = buffers.base_vertex;
  snap.d3d_start_index = buffers.start_index;
  snap.d3d_index_32bit = buffers.index_32bit;
  for (uint32_t s = 0; s < 4; ++s) {
    snap.d3d_stream_address[s] = buffers.stream_address[s];
    snap.d3d_stream_stride[s] = buffers.stream_stride[s];
  }
  snap.d3d_target_width = buffers.target_width;
  snap.d3d_target_height = buffers.target_height;
  if (buffers.vertex_buffer != 0) {
    d3d_snapshots_seen_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t n = d3d_big_draw_log_count_.fetch_add(1);
    if (n < 8 || (n % 20000) == 0) {
      REXLOG_INFO("plume: draw joined: indices={} vb={:08X}+{} ib={:08X} vs={:016X} ps={:016X}",
                  num_indices, buffers.vertex_buffer, buffers.vertex_stride,
                  buffers.index_buffer, snap.vs_hash, snap.ps_hash);
    }
  }
  snap.prim_type = prim_type;
  snap.source_select = source_select;
  snap.num_indices = num_indices;
  snap.vs_constants.AssignShared(&gpu_registers_[0x4000], last_vs_constants_);
  snap.ps_constants.AssignShared(&gpu_registers_[0x4400], last_ps_constants_);
  snap.fetch_constants.AssignShared(&gpu_registers_[0x4800], last_fetch_constants_);
  // A draw built from the Direct3D calls has no constants in the register
  // file: this path never receives a SET_CONSTANT packet, so what the ring
  // left there belongs to the interface. The title's own copy does have them,
  // at the offsets the reference implementation asserts - 0x700 and 0x1700.
  //
  // Per draw, not per frame: the transform differs from object to object, so
  // a single pull per frame would give every mesh the same matrix.
  //
  // Tell these draws apart by the buffer they came with. Selecting them by the
  // packet's source_select field instead picked out the interface's own
  // auto-indexed draws - that field describes a packet, not where a draw came
  // from - and overwriting their constants cost the menu its render.
  if (snap.d3d_vertex_buffer != 0) {
    const uint32_t d3d_device = d3d_device_guest_.load(std::memory_order_relaxed);
    if (d3d_device != 0 && DeviceRangeReadable(memory_, d3d_device)) {
      // Already here when this snapshot pulled them from the same device at
      // its start - the register file it was copied from holds them. Copying
      // them again, byte-swapped, was 8 KB per draw for nothing, a thousand
      // draws a frame, on the title's thread.
      if (d3d_device != device) {
        // Read into scratch and taken as shared blocks, like the register
        // file's: written into the snapshot's own block instead, every draw
        // copied the block it shared first - an allocation and 4 KB twice per
        // draw on the title's thread, and the frees half the present thread's
        // allocator work on the Pixel.
        static thread_local std::array<uint32_t, 1024> vs_scratch;
        static thread_local std::array<uint32_t, 1024> ps_scratch;
        CopyDeviceDwords(memory_, vs_scratch.data(), d3d_device, kD3DVsFloatOffset, 1024);
        CopyDeviceDwords(memory_, ps_scratch.data(), d3d_device, kD3DPsFloatOffset, 1024);
        snap.vs_constants.AssignShared(vs_scratch.data(), last_device_vs_constants_);
        snap.ps_constants.AssignShared(ps_scratch.data(), last_device_ps_constants_);
      }
      // The boolean constants steer the shaders' branches, and the ring's
      // copy belongs to the interface just like its float constants do.
      CopyDeviceDwords(memory_, &snap.vs_bool, d3d_device, kD3DVsBoolOffset, 1);
      CopyDeviceDwords(memory_, &snap.ps_bool, d3d_device, kD3DPsBoolOffset, 1);
    }
  }
  // The textures the draw was made with. Reading the device's own fetch
  // constants was tried and does not work for this title: the offset the
  // reference implementation asserts holds something else here, and what came
  // back was a one-by-one texture at a code address. The objects the title
  // bound are reliable, and each carries its six-word descriptor at +0x1C -
  // the same shape as the vertex buffer's, which is already relied on.
  // The fetch constants Direct3D has prepared for the GPU, from the device.
  // Taken from the texture objects instead, a texture in the GPU window kept
  // its virtual address - and read as physical that lands a page (0x1000)
  // short, a quarter tile into a tiled texture: glyphs came out as other
  // glyphs and the car's livery in shuffled strips.
  bool fetch_from_device = false;
  if (snap.d3d_vertex_buffer != 0 && memory_) {
    const uint32_t d3d_device = d3d_device_guest_.load(std::memory_order_relaxed);
    if (d3d_device != 0 && DeviceRangeReadable(memory_, d3d_device)) {
      // Pulled from this device at the snapshot's start already, like the
      // float constants above; read again only if the device changed since.
      std::array<uint32_t, 192> from_device = snap.fetch_constants;
      if (d3d_device != device) {
        CopyDeviceDwords(memory_, from_device.data(), d3d_device, kD3DFetchOffset, 192);
      }
      static std::atomic<uint32_t> compared{0};
      if (compared.fetch_add(1, std::memory_order_relaxed) < 6) {
        const uint32_t object = buffers.textures[0];
        uint32_t object_word1 = 0;
        if (object >= 0x10000000u) {
          CopyBeDwords(memory_, &object_word1, object + 0x14, 1);
        }
        REXLOG_INFO("plume: texture slot 0 base word - device {:08X}, object {:08X}",
                    from_device[1], object_word1);
      }
      if (d3d_device != device) {
        snap.fetch_constants.AssignShared(from_device.data(), last_device_fetch_constants_);
      }
      fetch_from_device = true;
    }
  }
  if (snap.d3d_vertex_buffer != 0 && memory_ && !fetch_from_device) {
    // Start from nothing. What the ring left in these belongs to the
    // interface, and a slot the scene never bound was picking it up - which
    // read as a bound 512x512 texture on every scene draw and hid the fact
    // that the slot the scene actually uses was being dropped.
    snap.fetch_constants.fill(0);
    for (uint32_t slot = 0; slot < 8; ++slot) {
      const uint32_t texture = buffers.textures[slot];
      // The scene's textures live in the window the GPU reads through, the
      // interface's in the title's heap. Excluding the window dropped every
      // scene texture - the same mistake the vertex buffers took.
      if (texture < 0x10000000u) {
        continue;
      }
      const uint32_t words = slot * 6;
      if (words + 6 > snap.fetch_constants.size()) {
        break;
      }
      // The descriptor sits at +0x10 in this build, not the +0x1C the
      // reference implementation asserts - its resource header is four words
      // longer. Both the heap textures and the ones in the GPU window agree on
      // it, and the type bits read 2, which is what a texture descriptor says.
      CopyBeDwords(memory_, snap.fetch_constants.data() + words, texture + 0x10, 6);
    }
  }
  if (snap.d3d_vertex_buffer == 0) {
    snap.vs_bool = gpu_registers_[0x4900];
    snap.ps_bool = gpu_registers_[0x4904];
  }
  snap.vte_cntl = gpu_registers_[0x2206];
  // RB_BLENDCONTROL0 / RB_COLOR_MASK - the passthrough pipeline reproduces
  // these instead of assuming one fixed alpha blend for every draw.
  snap.blend_control = gpu_registers_[0x2201];
  snap.color_mask = gpu_registers_[0x2104];
  snap.depth_control = gpu_registers_[0x2200];  // RB_DEPTHCONTROL
  // A scene draw takes its render state from the device as well. The device
  // keeps a contiguous shadow of the render-backend registers, register R at
  // device + 0x2D74 + (R - 0x2200) * 4: D3DDevice_SetRenderState_ZEnable,
  // ZWriteEnable and ZFunc all edit the word at 0x2D74 (bits 1, 2 and 4-6 -
  // RB_DEPTHCONTROL's own layout), SrcBlend and DestBlend the one at 0x2D78.
  //
  // What the ring left in these belongs to the interface, which draws with
  // depth off. With that state the car's back faces were drawn over its front
  // ones, which is what the spinning artifact was.
  if (snap.d3d_vertex_buffer != 0) {
    const uint32_t device = d3d_device_guest_.load(std::memory_order_relaxed);
    if (device != 0 && RangeReadable(memory_, device + kD3DRenderStateOffset, 0x1C)) {
      uint32_t state[2] = {};
      CopyBeDwords(memory_, state, device + kD3DRenderStateOffset, 2);
      snap.depth_control = state[0];
      snap.blend_control = state[1];
      // RB_COLORCONTROL (0x2202): alpha function in bits 0-2, the test's enable
      // in bit 3 (D3DDevice_SetRenderState_AlphaTestEnable / AlphaFunc). The
      // shader can only clip below a threshold, so the "greater" functions
      // are the ones taken; the reference is kept as a float at 0x2D44.
      uint32_t colorcontrol = 0;
      CopyBeDwords(memory_, &colorcontrol, device + kD3DRenderStateOffset + 8, 1);
      const uint32_t alpha_func = colorcontrol & 0x7u;
      {
        static std::mutex seen_mutex;
        static std::set<uint32_t> seen;
        std::lock_guard lock(seen_mutex);
        if (seen.size() < 24 && seen.insert(colorcontrol).second) {
          uint32_t ref_bits = 0;
          CopyBeDwords(memory_, &ref_bits, device + 0x2D44, 1);
          float ref = 0.0f;
          std::memcpy(&ref, &ref_bits, 4);
          REXLOG_INFO("plume: RB_COLORCONTROL {:08X} (alpha func {} test {}) ref {:.3f} vs={:016X} "
                      "ps={:016X}",
                      colorcontrol, alpha_func, (colorcontrol >> 3) & 1, ref, snap.vs_hash,
                      snap.ps_hash);
        }
      }
      if (RangeReadable(memory_, device + 0x2D88, 4) && RangeReadable(memory_, device + 0x2E90, 8)) {
        uint32_t sc_mode = 0;
        CopyBeDwords(memory_, &sc_mode, device + 0x2D88, 1);
        snap.cull = sc_mode & 7u;
        if ((sc_mode >> 11) & 1u) {
          uint32_t offset_bits[2] = {};
          CopyBeDwords(memory_, offset_bits, device + 0x2E90, 2);
          std::memcpy(&snap.poly_offset_scale, &offset_bits[0], 4);
          std::memcpy(&snap.poly_offset_offset, &offset_bits[1], 4);
          snap.poly_offset = snap.poly_offset_scale != 0.0f || snap.poly_offset_offset != 0.0f;
          static std::atomic<uint32_t> shown{0};
          if (snap.poly_offset && shown.fetch_add(1, std::memory_order_relaxed) < 8) {
            REXLOG_INFO("plume: polygon offset scale {:g} offset {:g} vs={:016X} ps={:016X}",
                        snap.poly_offset_scale, snap.poly_offset_offset, snap.vs_hash,
                        snap.ps_hash);
          }
        }
      }
      snap.alpha_test = (colorcontrol & 0x8u) != 0 && (alpha_func == 4u || alpha_func == 6u);
      if (snap.alpha_test && RangeReadable(memory_, device + 0x2D44, 4)) {
        uint32_t ref_bits = 0;
        CopyBeDwords(memory_, &ref_bits, device + 0x2D44, 1);
        std::memcpy(&snap.alpha_ref, &ref_bits, 4);
      }
      // The scissor: the rectangle D3DDevice_SetScissorRect stores at +0x3220
      // (left, top, right, bottom) and the render state that enables it at
      // +0x2EB0. The interface clips its scrolling song titles with it.
      if (RangeReadable(memory_, device + 0x3220, 16) && RangeReadable(memory_, device + 0x2EB0, 4)) {
        uint32_t enabled = 0;
        CopyBeDwords(memory_, &enabled, device + 0x2EB0, 1);
        if (enabled != 0) {
          uint32_t rect[4] = {};
          CopyBeDwords(memory_, rect, device + 0x3220, 4);
          snap.scissor_enabled = true;
          for (int i = 0; i < 4; ++i) {
            snap.scissor_rect[i] = int32_t(rect[i]);
          }
        }
      }
      // PA_CL_VTE_CNTL (0x2206) from the same block, if the device keeps it.
      uint32_t vte = 0;
      CopyBeDwords(memory_, &vte, device + kD3DRenderStateOffset + (0x2206 - 0x2200) * 4, 1);
      snap.vte_cntl = vte;
      // RB_COLOR_MASK has its own word: D3DDevice_SetRenderState_
      // ColorWriteEnable edits the low four bits of device + 0x2D1C (its twin
      // for the second target the next four). The ring's copy was whatever the
      // ring last set - zero in the menus, which is why every interface draw
      // there reached the GPU and wrote nothing.
      uint32_t color_mask = 0;
      CopyBeDwords(memory_, &color_mask, device + kD3DColorMaskOffset, 1);
      snap.color_mask = color_mask;
      // Render target slot 1 (device + 0x30F8 + 4 * slot): bound while the
      // scene is drawn, and its shaders write their second output into it.
      if (RangeReadable(memory_, device + 0x30FC, 4)) {
        uint32_t rt1 = 0;
        CopyBeDwords(memory_, &rt1, device + 0x30FC, 1);
        snap.rt1_bound = rt1 != 0;
      }
      static std::atomic<uint32_t> shown{0};
      if (shown.fetch_add(1, std::memory_order_relaxed) < 4) {
        const float* vc = reinterpret_cast<const float*>(snap.vs_constants.data());
        const float* pc = reinterpret_cast<const float*>(snap.ps_constants.data());
        REXLOG_INFO("plume: scene render state from the device: depth={:08X} blend={:08X} "
                    "fog params c14=({:.3f},{:.3f},{:.3f},{:.3f}) fog colour ps c0=({:.3f},{:.3f},"
                    "{:.3f},{:.3f}) specularity ps c3=({:.3f},{:.3f},{:.3f},{:.3f})",
                    state[0], state[1], vc[56], vc[57], vc[58], vc[59], pc[0], pc[1], pc[2],
                    pc[3], pc[12], pc[13], pc[14], pc[15]);
      }
    }
  }
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

  uint64_t video_key = 0;
  const auto fill_done = clock_now();
  // The video path captures a frame and then keeps presenting it until told
  // otherwise, which is right for a menu backdrop and wrong the moment the
  // title moves on to something not recognised as a new frame - the stale
  // frame then sits over everything. Switching the player off outright says at
  // once whether what is underneath is being drawn at all.
  static const bool video_disabled = std::getenv("XERENGE_NO_VIDEO") != nullptr;
  const bool is_video =
      !video_disabled && snap.valid && IsGuestVideoBlit(snap, &video_key);
  if (timed) {
    snapshot_classify_ns_.fetch_add(
        16 * std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - fill_done)
                 .count(),
        std::memory_order_relaxed);
    snapshot_fill_ns_.fetch_add(
        16 * std::chrono::duration_cast<std::chrono::nanoseconds>(fill_done - pull_done).count(),
        std::memory_order_relaxed);
  }
  if (is_video && memory_) {
    const auto now = std::chrono::steady_clock::now();
    // Decoding the same frame again produces the same bytes, so the only thing
    // worth decoding is a frame we have not seen. The key identifies the source
    // frame, and the two-millisecond floor alone let the same one be decoded
    // hundreds of times a second - measured at 4.2 seconds of a 40 second run,
    // spent on the guest thread, which is essentially the whole cost of the
    // ring walk.
    const bool already_have_this_frame =
        video_key != 0 && video_key == last_video_key_.load(std::memory_order_relaxed) &&
        pending_video_.has_video_frame();
    if (!already_have_this_frame &&
        (last_video_capture_.time_since_epoch().count() == 0 ||
         now - last_video_capture_ >= std::chrono::milliseconds(2))) {
      last_video_capture_ = now;
      // Decoding a frame costs about ten milliseconds, and doing it here spends
      // them on a guest thread, inside the ring walk - a hitch nine times a
      // second, landing hardest exactly when the scene starts and the title can
      // least afford it. Hand the request to a worker and carry on; the frame
      // appears a beat later, which for a background video is not something
      // anyone can see.
      RequestVideoCapture(snap);
    }
  }

  std::lock_guard lock(snapshot_mutex_);
  if (is_video) {
    ++g_video_classified_since_present;
    video_seen_ = true;
    static const bool targets_followed = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
    if (targets_followed) {
      GuestDrawSnapshot slot;
      slot.video_slot = true;
      draw_ring_[draw_ring_next_] = std::move(slot);
      draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
      if (draw_ring_count_ < kDrawRingSize) {
        ++draw_ring_count_;
      }
    }
    if (snap.has_video_frame()) {
      // The "weaker frame" test exists to skip the odd mid-decode grey frame,
      // but it measures against the frame currently on screen, and that frame
      // only changes when a frame is accepted. So one rejection makes every
      // darker frame afterwards get compared with the same bright one, and
      // the picture latches there for good. A fade to black at the end of an
      // intro logo is precisely that: sustained, entirely legitimate
      // darkening. Skipping an isolated frame is still worth doing; refusing
      // several in a row is just the video freezing, so let those through.
      // None of that reasoning survives a change of clip. Comparing the first
      // frame of one video against the last frame of another compares nothing
      // meaningful, and an intro that opens on a fade-in reads as "weaker"
      // than the menu background it replaced - so its opening frames get
      // refused and the previous clip's frame is shown again in their place,
      // which is what made the two strobe against each other.
      const uint64_t incoming_key = snap.video_key ? snap.video_key : video_key;
      const bool new_clip = incoming_key != last_video_key_.load(std::memory_order_relaxed);
      const bool weaker = !new_clip && VideoFrameWeakerThan(snap, pending_video_);
      if (!weaker || video_frames_rejected_ >= 2) {
        if (weaker) {
          REXLOG_INFO("plume: accepting dimmer video frame after {} rejections (mean {} range {})",
                      video_frames_rejected_, snap.video_luma_mean, snap.video_luma_range);
        }
        video_frames_rejected_ = 0;
        last_video_key_.store(incoming_key, std::memory_order_relaxed);
        pending_video_ = std::move(snap);
      } else {
        ++video_frames_rejected_;
      }
    }
    return;
  }

  // Accounting for what reaches the overlay ring, so a frame that is missing
  // UI can be told apart from one whose draws never arrived. Guarded by
  // snapshot_mutex_, which is held here and where they are read.
  ++g_ring_pushed_since_present;
  if (!(snap.valid && num_indices >= 3)) {
    ++g_ring_rejected_since_present;
    // Three quite different failures land here - no vertex shader bound, no
    // pixel shader bound, or too few indices to form a triangle - and they
    // call for three different fixes, so count them apart.
    if (snap.vs_hash == 0) {
      ++g_ring_rejected_no_vs;
    } else if (snap.ps_hash == 0) {
      ++g_ring_rejected_no_ps;
    } else {
      ++g_ring_rejected_few_indices;
    }
  }
  // With the interface drawn from the Direct3D calls, the ring's own draws are
  // the same draws a second time - skip them, except the video the movie
  // player puts through it.
  //
  // Only DRAW_INDX, though. Every Direct3D call that is followed here -
  // BeginVertices, DrawVertices, DrawIndexedVertices - writes that packet, per
  // the beta's code. DRAW_INDX_2, with its indices inline, comes from calls
  // that are not followed (the menus' own drawing among them), so for those the
  // ring is the only source: dropping them too left the menus blank.
  static const bool interface_from_d3d = std::getenv("XERENGE_D3D_UI") != nullptr;
  const bool from_draw_indx = ring_draw_opcode_ == 0x22 || ring_draw_opcode_ == 0x34;
  const bool ring_duplicate = interface_from_d3d && snap.d3d_vertex_buffer == 0 &&
                              !snap.has_video_frame() && from_draw_indx;
  if (snap.valid && num_indices >= 3 && !ring_duplicate) {
    if (snap.d3d_vertex_buffer != 0) {
      d3d_snapshots_pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    // Moved, not copied: nothing reads the snapshot after this, and the copy
    // (a few hundred bytes and three shared blocks per draw) was most of the
    // title thread's memmove on the Pixel.
    draw_ring_[draw_ring_next_] = std::move(snap);
    draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
    if (draw_ring_count_ < kDrawRingSize) {
      ++draw_ring_count_;
    } else {
      // The ring is full: this snapshot has just overwritten one that had not
      // been presented yet, so that draw is gone from the frame.
      ++g_ring_overwritten_since_present;
    }
  }
}

void PlumeGraphicsSystem::NoteShaderLoad(uint32_t object, bool pixel_shader, uint32_t microcode,
                                         uint32_t size) {
  static const bool enabled = std::getenv("XERENGE_D3D_DRAWS") != nullptr;
  if (!enabled || !object || !microcode || !memory_) {
    return;
  }
  // Real shaders are small. Anything else means the fields were not what they
  // are taken for, and hashing it would read memory that is not a shader.
  if (size == 0 || size > 0x10000 || (size & 3) != 0 || !RangeReadable(memory_, microcode, size)) {
    return;
  }
  const auto* bytes = memory_->TranslateVirtual<const uint8_t*>(microcode);
  if (!bytes) {
    return;
  }
  const uint64_t hash = XXH3_64bits(bytes, size);
  const ShaderMicrocodeEntry* micro = PlumeShaderCache::Instance().FindByMicrocode(hash);
  // The cache records the stage each microcode was translated as (0 vertex,
  // 1 pixel). A match of the other stage is not this shader.
  if (micro && (micro->stage != 0) != pixel_shader) {
    static std::atomic<uint32_t> mismatched{0};
    if (mismatched.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_WARN("plume: Direct3D loaded {} {:08X} microcode {:016X} is cached as the other "
                  "stage - ignored",
                  pixel_shader ? "PS" : "VS", object, hash);
    }
    return;
  }
  if (!micro) {
    // Not translated: this exact microcode - after Direct3D's patching - is
    // not in the cache, so there is no shader to draw it with. Dump it for the
    // offline translation (tools/translate_runtime_shaders.sh), exactly as the
    // ring's loads are dumped. The scene's shaders never pass through the ring,
    // which is why none of them had ever been dumped or translated.
    //
    // Once per hash: this runs on the title's own thread and a load repeats on
    // every draw, so the file work must not.
    bool first_miss = false;
    {
      std::lock_guard lock(guest_shader_mutex_);
      first_miss = dumped_from_loads_.insert(hash).second;
    }
    if (first_miss) {
      DumpMicrocode(pixel_shader ? 1u : 0u, hash, bytes, size);
    }
    const uint64_t n = loads_unknown_.fetch_add(1, std::memory_order_relaxed);
    (void)n;
    if (first_miss) {
      REXLOG_WARN("plume: Direct3D loaded {} {:08X} microcode {:016X} ({} bytes) - not in the "
                  "shader cache",
                  pixel_shader ? "PS" : "VS", object, hash, size);
    }
    return;
  }
  const uint64_t resolved = micro->shaderHash ? micro->shaderHash : hash;
  bool fresh_layout = false;
  {
    std::lock_guard lock(guest_shader_mutex_);
    guest_shaders_[object] = GuestShaderSlot{resolved, pixel_shader};
    if (!pixel_shader) {
      fresh_layout = layouts_from_loads_.insert(resolved).second;
    }
  }
  // A shader first loaded while the scene is up never went through the ring,
  // so its vertex fetch layout was never read. Read it from the same bytes.
  if (fresh_layout && draw_context_) {
    draw_context_->RegisterVsUcode(resolved, bytes, size);
  }
  const uint64_t n = loads_resolved_.fetch_add(1, std::memory_order_relaxed);
  if (n < 12) {
    REXLOG_INFO("plume: Direct3D loaded {} {:08X} -> shader {:016X} ({} bytes)",
                pixel_shader ? "PS" : "VS", object, resolved, size);
  }
}

void PlumeGraphicsSystem::PushMarker(GuestDrawSnapshot marker) {
  std::lock_guard lock(snapshot_mutex_);
  draw_ring_[draw_ring_next_] = std::move(marker);
  draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
  if (draw_ring_count_ < kDrawRingSize) {
    ++draw_ring_count_;
  }
}

// With every render target drawn into the one buffer, as the console draws
// every target into the one EDRAM, the title's own clears are what separate
// one target's picture from the next - colour as well as depth.
void PlumeGraphicsSystem::NoteGuestClearColor(uint32_t flags, const float color[4],
                                              bool whole_target, float depth) {
  if ((flags & 0x1Fu) == 0) {
    return;
  }
  GuestDrawSnapshot marker;
  marker.is_clear = true;
  // A smaller target's size, packed into the high bits by the ClearF hook.
  if ((flags & 0x40000000u) != 0) {
    marker.clear_width = ((flags >> 8) & 0x7FFu) + 1;
    marker.clear_height = ((flags >> 19) & 0x7FFu) + 1;
    flags &= 0xFFu;
  }
  marker.clear_flags = flags;
  marker.clear_depth = depth;
  marker.clear_whole = whole_target;
  for (int i = 0; i < 4; ++i) {
    marker.clear_color[i] = color[i];
  }
  PushMarker(std::move(marker));
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("plume: title clears flags {:X} colour ({:.2f},{:.2f},{:.2f},{:.2f}) depth {} {}",
                flags, color[0], color[1], color[2], color[3], depth,
                whole_target ? "whole target" : "rectangles");
  }
}

void PlumeGraphicsSystem::NoteGuestFrameEnd() {
  static const bool targets = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  if (!targets) {
    return;
  }
  GuestDrawSnapshot marker;
  marker.is_frame_end = true;
  PushMarker(std::move(marker));
}

// Marks where the copy happens in the draw order: the render target as it is
// now becomes the picture later draws sample at dest_physical.
void PlumeGraphicsSystem::NoteGuestResolve(uint32_t flags, uint32_t dest_physical,
                                           uint32_t source_width, uint32_t source_height,
                                           uint32_t face, bool cube) {
  if (dest_physical == 0) {
    return;
  }
  GuestDrawSnapshot marker;
  marker.is_resolve = true;
  marker.is_end_tiling = (flags & 0x80000000u) != 0;
  marker.resolve_flags = flags;
  marker.resolve_dest = dest_physical;
  marker.resolve_source = flags & 0x7u;
  marker.resolve_width = source_width;
  marker.resolve_height = source_height;
  marker.resolve_face = face;
  marker.resolve_cube = cube;
  PushMarker(std::move(marker));
  // Once per destination: the menus resolve into the same two front buffers
  // every frame, and a cap on the first dozen lines never reached the scene.
  static std::mutex seen_mutex;
  static std::unordered_set<uint32_t> seen;
  bool fresh = false;
  {
    std::lock_guard lock(seen_mutex);
    fresh = seen.size() < 64 && seen.insert(dest_physical).second;
  }
  if (fresh) {
    REXLOG_INFO("plume: title resolves (flags {:X}{}) into {:08X} - first time, from {}x{}{}",
                flags & 0xFFFFu, (flags & 0x80000000u) ? ", EndTiling" : "", dest_physical,
                source_width, source_height, cube ? fmt::format(", cube face {}", face) : "");
  }
}

void PlumeGraphicsSystem::NoteGuestClear(uint32_t flags, float depth, uint32_t stencil) {
  (void)stencil;
  static const bool enabled = std::getenv("XERENGE_D3D_DRAWS") != nullptr;
  // Only depth matters here. The scene tests with GEQUAL and clears depth to
  // its own value; a depth buffer that only ever holds the 1.0 it is cleared
  // to at the start of the frame fails every one of those tests.
  if (!enabled || (flags & 0x10u) == 0) {
    return;
  }
  GuestDrawSnapshot marker;
  marker.is_clear = true;
  marker.clear_flags = flags;
  marker.clear_depth = depth;
  {
    std::lock_guard lock(snapshot_mutex_);
    draw_ring_[draw_ring_next_] = std::move(marker);
    draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
    if (draw_ring_count_ < kDrawRingSize) {
      ++draw_ring_count_;
    }
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("plume: title clears depth to {} (flags {:X})", depth, flags);
  }
}

bool PlumeGraphicsSystem::PresentedRecently() const {
  const uint64_t last = last_present_ms_.load(std::memory_order_relaxed);
  if (last == 0) {
    return true;
  }
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  // Three frames' grace at sixty. Long enough that the ring keeps ownership
  // whenever it is working at all, short enough that a stall is caught before
  // the screen visibly freezes.
  return now - last < 50;
}

namespace {
// The vsync worker produces the vblanks; a swap it happens to process while
// pumping the ring must not wait for one.
thread_local bool tl_vsync_worker = false;
}  // namespace

void PlumeGraphicsSystem::PaceSwapToVblank() {
  static const bool off = std::getenv("XERENGE_NO_VBLANK_PACE") != nullptr ||
                          std::getenv("XERENGE_UNLOCK_FPS") != nullptr;
  if (off || tl_vsync_worker) {
    return;
  }
  const auto wait_started = std::chrono::steady_clock::now();
  std::unique_lock lock(vblank_mutex_);
  // At most one swap per vblank: if one has passed since the last swap, the
  // frame goes at once, as a late frame flips at the next vblank on the
  // console; otherwise wait for it. Bounded, so a stalled vsync worker cannot
  // hang the title.
  // Crash mode is paced at every other vblank, as the console presents it: the
  // title then steps its logic twice a frame, and swapped at every vblank it
  // ran at double speed. The title's own clock says which (game_timing.cpp).
  uint32_t interval = 1;
  if (const RexFrameClockProviderFn clock_provider = RexFrameClockProvider()) {
    struct {
      uint64_t frame;
      uint32_t steps_this_frame, epoch;
      double state_steps, real_steps;
      uint32_t valid, vblanks_per_frame;
    } clock{};
    clock_provider(&clock, sizeof(clock));
    interval = clock.vblanks_per_frame == 2 ? 2u : 1u;
  }
  vblank_cv_.wait_for(lock, std::chrono::milliseconds(100), [this, interval] {
    return vblank_count_.load(std::memory_order_acquire) >= last_swap_vblank_ + interval;
  });
  last_swap_vblank_ = vblank_count_.load(std::memory_order_acquire);
  {
    // Time left over in a frame: how long the title waited here. What the
    // title and the renderer took between two waits is the rest.
    static uint64_t waited_us = 0, swaps = 0;
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    waited_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - wait_started)
                              .count());
    ++swaps;
    if (now - last_report >= std::chrono::seconds(2)) {
      REXLOG_INFO("plume: frame headroom - waited {} us a frame for the vblank ({} swaps)",
                  waited_us / swaps, swaps);
      waited_us = 0;
      swaps = 0;
      last_report = now;
    }
  }
}

void PlumeGraphicsSystem::PresentGuestFrame(uint32_t width, uint32_t height) {
  if (!app_context_ || !swapchain_ || !swapchain_->IsReady()) {
    return;
  }
#if defined(__ANDROID__)
  g_title_tid.store(int(gettid()), std::memory_order_relaxed);
#endif
  PaceSwapToVblank();
  last_present_ms_.store(static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count()),
                         std::memory_order_relaxed);

  const uint64_t presented_frame = frame_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  // A RenderDoc capture of chosen frames, without anyone at the keyboard:
  // XERENGE_CAPTURE_FRAME=<n>[,<n>...] asks RenderDoc for the next frame once
  // this many have been presented.
  if (renderdoc_api_ && renderdoc_api_->api_1_0_0()) {
    static const std::vector<uint64_t> wanted = [] {
      std::vector<uint64_t> frames;
      if (const char* list = std::getenv("XERENGE_CAPTURE_FRAME")) {
        std::string text(list);
        size_t from = 0;
        while (from < text.size()) {
          const size_t comma = text.find(',', from);
          frames.push_back(std::strtoull(text.substr(from, comma - from).c_str(), nullptr, 10));
          if (comma == std::string::npos) {
            break;
          }
          from = comma + 1;
        }
      }
      return frames;
    }();
    if (std::find(wanted.begin(), wanted.end(), presented_frame) != wanted.end()) {
      renderdoc_api_->api_1_0_0()->TriggerCapture();
      REXLOG_INFO("plume: RenderDoc capture requested at frame {}", presented_frame);
    }
    // XERENGE_CAPTURE_SECONDS=<s>[,<s>...]: the same, by time since the first
    // present - frame counts drift between runs, the replay's timing does not.
    static const std::vector<double> wanted_seconds = [] {
      std::vector<double> times;
      if (const char* list = std::getenv("XERENGE_CAPTURE_SECONDS")) {
        std::string text(list);
        size_t from = 0;
        while (from < text.size()) {
          const size_t comma = text.find(',', from);
          times.push_back(std::strtod(text.substr(from, comma - from).c_str(), nullptr));
          if (comma == std::string::npos) {
            break;
          }
          from = comma + 1;
        }
      }
      return times;
    }();
    // A capture just written (F12 or requested): log the draw queue of the
    // next frame beside its file name. The screens captured by hand are the
    // still ones, so the next frame is the same picture.
    {
      static uint32_t captures_seen = 0;
      const uint32_t captures = renderdoc_api_->api_1_0_0()->GetNumCaptures();
      if (captures > captures_seen && draw_context_) {
        char path[1024] = {};
        uint32_t length = sizeof(path);
        uint64_t timestamp = 0;
        renderdoc_api_->api_1_0_0()->GetCapture(captures - 1, path, &length, &timestamp);
        draw_context_->RequestQueueDump(path);
        call_trace_requested_.store(true);
      }
      captures_seen = captures;
    }
    if (!wanted_seconds.empty()) {
      static const auto first_present = std::chrono::steady_clock::now();
      static size_t next_capture = 0;
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - first_present).count();
      if (next_capture < wanted_seconds.size() && elapsed >= wanted_seconds[next_capture]) {
        ++next_capture;
        renderdoc_api_->api_1_0_0()->TriggerCapture();
        REXLOG_INFO("plume: RenderDoc capture requested at {:.1f} s", elapsed);
      }
    }
  }

  // VdSwap calls this on a guest thread, so waiting for the UI thread to
  // finish drawing spends the title's own time: measured at 12 ms a frame.
  // Against xenos on the same screen the title kicks 13131 times to our 1803 -
  // seven times less work gets submitted - and that is what walks it into the
  // one-lap limit its flow control then blocks on. So hand the frame over and
  // let it run.
  //
  // One frame in flight at a time: fire-and-forget with no bound would queue
  // frames faster than they can be drawn whenever the title outruns the
  // display, and every one of those is a frame nobody sees.
  // Dropping a frame when one is already in flight is not pacing, it is no
  // pacing at all: the title's swap returns instantly every time and it runs
  // as fast as it can - measured at 207117 kicks against xenos's 13131 - while
  // overwriting the very buffers being drawn from. Wait for the PREVIOUS frame
  // instead. That still keeps one frame in flight, so the title runs while we
  // draw, but it can never get more than a frame ahead of what is on screen.
  static const bool wait_for_present = std::getenv("XERENGE_SYNC_PRESENT") != nullptr;
  if (!wait_for_present) {
    StartPresentWorker();
    {
      const auto wait_started = std::chrono::steady_clock::now();
      std::unique_lock lock(present_flight_mutex_);
      const bool ready = present_flight_cv_.wait_for(lock, std::chrono::milliseconds(2000),
                                                     [this] { return !present_in_flight_; });
      // The title's own thread, blocked on this renderer: per second, so a
      // frozen scene change shows whether the time went here or elsewhere.
      {
        static std::chrono::steady_clock::time_point second_started = wait_started;
        static uint64_t waited_us = 0;
        static uint32_t swaps = 0;
        const auto now = std::chrono::steady_clock::now();
        waited_us += uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(now - wait_started).count());
        ++swaps;
        if (now - second_started >= std::chrono::seconds(1)) {
          if (waited_us >= 100000) {
            REXLOG_WARN("plume: VdSwap waited {} ms for the previous frame in {} swaps", waited_us / 1000,
                        swaps);
          }
          second_started = now;
          waited_us = 0;
          swaps = 0;
        }
      }
      if (!ready) {
        REXLOG_WARN("plume: present_flight timed out waiting for previous frame");
        return;
      }
      present_in_flight_ = true;
    }
    {
      std::lock_guard lock(present_request_mutex_);
      present_request_width_ = width;
      present_request_height_ = height;
      present_request_pending_ = true;
    }
    present_request_cv_.notify_one();
    return;
  }

  // Fallback for synchronous present.
  PresentClearColorOnUiThread(width, height);
}

void PlumeGraphicsSystem::PresentClearColorOnUiThread(uint32_t guest_width,
                                                        uint32_t guest_height) {
  (void)guest_width;
  (void)guest_height;

  if (!swapchain_ || !swapchain_->IsReady()) {
    return;
  }

  // This runs on the present worker (on a guest thread through VdSwap only
  // with XERENGE_SYNC_PRESENT). Either way the title waits for it at its next
  // VdSwap once it takes longer than a frame, so measure it rather than assume
  // it is free.
  const auto present_started = std::chrono::steady_clock::now();
  struct PresentTimer {
    std::chrono::steady_clock::time_point started;
    std::atomic<uint32_t>* counter;
    ~PresentTimer() {
      const auto took = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
      // The totals, not just the slow ones. A single 587 ms walk says nothing
      // about whether that is one bad frame or every frame, and the difference
      // decides whether this needs moving off the guest thread or simply
      // fixing where it is.
      {
        static std::atomic<uint64_t> total_us{0};
        static std::atomic<uint64_t> walks{0};
        static std::atomic<uint64_t> slowest_us{0};
        static std::atomic<uint64_t> last_ms{0};
        total_us.fetch_add(static_cast<uint64_t>(took), std::memory_order_relaxed);
        const uint64_t n = walks.fetch_add(1, std::memory_order_relaxed) + 1;
        uint64_t was_slowest = slowest_us.load(std::memory_order_relaxed);
        while (static_cast<uint64_t>(took) > was_slowest &&
               !slowest_us.compare_exchange_weak(was_slowest, static_cast<uint64_t>(took))) {
        }
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        uint64_t was = last_ms.load(std::memory_order_relaxed);
        if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
          const uint64_t total = total_us.load(std::memory_order_relaxed);
          REXLOG_INFO("plume: frame presents {}, {} ms total, {} us each, slowest {} us", n,
                      total / 1000, total / (n ? n : 1),
                      slowest_us.load(std::memory_order_relaxed));
        }
      }
      if (took < 8000) {
        return;
      }
      const uint32_t n = counter->fetch_add(1);
      if (n < 8 || (n % 120) == 0) {
        REXLOG_WARN("plume: present took {} us", took);
      }
    }
  } present_timer{present_started, &slow_present_log_count_};

  std::lock_guard lock(present_mutex_);
  const auto present_locked = std::chrono::steady_clock::now();
  g_present_encode_us = 0;

  // Kept from frame to frame (cleared, not freed): a frame's snapshots are a
  // few hundred KB, which Android's allocator maps and unmaps at that size -
  // the madvise calls were an eighth of this thread on the Pixel.
  std::vector<GuestDrawSnapshot>& batch = present_batch_;
  batch.clear();
  {
    // Everything this mutex protects is written by the command processor one
    // draw at a time, so however long it is held here is time that thread
    // spends queueing instead of walking packets - and it was held across some
    // five megabytes of copying plus a formatted log line. Measured, that was
    // 43 us of waiting per draw and over four seconds across a run: nearly the
    // whole cost of the ring walk.
    //
    // So take it only to empty the ring, moving rather than copying (the
    // entries are abandoned the moment the count is reset), and sample the
    // counters into locals. The assembly below touches only this thread's own
    // state and does not need it.
    std::vector<GuestDrawSnapshot>& overlays = present_overlays_;
    overlays.clear();
    uint32_t pushed = 0, rejected = 0, no_vs = 0, no_ps = 0, few_indices = 0, overwritten = 0;
    uint32_t classified_video = 0;
    bool saw_video = false;
    {
      std::lock_guard snap_lock(snapshot_mutex_);
      if (draw_ring_count_ != 0) {
        const uint32_t start = (draw_ring_next_ + kDrawRingSize - draw_ring_count_) % kDrawRingSize;
        // Up to the last Swap the title made: what it issued after that is the
        // start of its next frame. Taken along, the next frame's first clear
        // and draws landed after this frame's final copy, the cleared target
        // was shown instead of the copy, and a black frame flashed up.
        uint32_t take = draw_ring_count_;
        bool found_end = false;
        for (uint32_t i = draw_ring_count_; i-- > 0;) {
          if (draw_ring_[(start + i) % kDrawRingSize].is_frame_end) {
            take = i + 1;
            found_end = true;
            break;
          }
        }
        // No Swap yet: the title is still drawing this frame. Taken anyway it
        // was shown as two clears and a few dozen draws - the pause menu
        // blinked to a black screen with a building or two on it, more often
        // the slower the device. Left for the present that finds its Swap,
        // unless the title is not swapping at all or the ring fills up.
        //
        // Twice in a row at most, and never with video playing: the intro and
        // the title screen present without a Swap in the ring, and held back
        // eight presents at a time their video started late and played in
        // fits - the title screen stayed black behind its logo.
        static const bool frame_ends_marked = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
        if (found_end) {
          presents_without_frame_end_ = 0;
        } else if (frame_ends_marked && presents_without_frame_end_ < 2 && !video_seen_ &&
                   draw_ring_count_ < kDrawRingSize / 2) {
          ++presents_without_frame_end_;
          take = 0;
        }
        overlays.reserve(take);
        for (uint32_t i = 0; i < take; ++i) {
          GuestDrawSnapshot& entry = draw_ring_[(start + i) % kDrawRingSize];
          if (!entry.is_frame_end) {
            overlays.push_back(std::move(entry));
          }
        }
        // What is left stays, moved to the front, for the next present.
        const uint32_t left = draw_ring_count_ - take;
        std::vector<GuestDrawSnapshot> rest;
        rest.reserve(left);
        for (uint32_t i = 0; i < left; ++i) {
          rest.push_back(std::move(draw_ring_[(start + take + i) % kDrawRingSize]));
        }
        for (uint32_t i = 0; i < left; ++i) {
          draw_ring_[i] = std::move(rest[i]);
        }
        draw_ring_count_ = left;
        draw_ring_next_ = left % kDrawRingSize;
      }
      pushed = g_ring_pushed_since_present;
      rejected = g_ring_rejected_since_present;
      no_vs = g_ring_rejected_no_vs;
      no_ps = g_ring_rejected_no_ps;
      few_indices = g_ring_rejected_few_indices;
      overwritten = g_ring_overwritten_since_present;
      classified_video = g_video_classified_since_present;
      g_ring_pushed_since_present = 0;
      g_ring_rejected_since_present = 0;
      g_ring_rejected_no_vs = 0;
      g_ring_rejected_no_ps = 0;
      g_ring_rejected_few_indices = 0;
      g_ring_overwritten_since_present = 0;
      g_video_classified_since_present = 0;
      saw_video = video_seen_;
      video_seen_ = false;
    }
    {
      // Capped at the first handful this only ever described the opening
      // frames - the screens worth measuring come minutes later - so keep
      // reporting at a low rate for the whole run.
      static uint32_t ring_logs = 0;
      const bool lost = overwritten != 0;
      ++ring_logs;
      if (ring_logs <= 16 || lost || (ring_logs % 120) == 0) {
        // How many of the kept draws came in through the Direct3D calls. The
        // counts above are all about the command ring, so a scene that draws
        // entirely through the other path is invisible in them - which is
        // exactly the case that needs watching.
        size_t from_d3d = 0;
        for (const auto& overlay : overlays) {
          if (overlay.d3d_vertex_buffer != 0) {
            ++from_d3d;
          }
        }
        REXLOG_INFO(
            "plume: ring {} draws this present (kept {}, {} from Direct3D, {} seen/{} pushed; "
            "rejected {} = {} no vs / {} no ps / {} too few indices, overwritten {}, video {})",
            pushed, overlays.size(), from_d3d,
            d3d_snapshots_seen_.load(std::memory_order_relaxed),
            d3d_snapshots_pushed_.load(std::memory_order_relaxed), rejected, no_vs, no_ps,
            few_indices, overwritten, classified_video);
      }
      if (lost) {
        REXLOG_WARN("plume: overlay ring overflowed, {} draw(s) lost from this frame", overwritten);
      }
    }
    if (saw_video) {
      no_video_presents_ = 0;
    } else {
      ++no_video_presents_;
    }
    // Only a short list is ever used again (below), so only a short one is
    // kept: copying the whole frame here - two thousand snapshots in a race,
    // constants and all - was a tenth of the present thread's time on the
    // Pixel, and as much again freeing it.
    if (!overlays.empty()) {
      if (overlays.size() < 16) {
        last_overlays_ = overlays;
      } else {
        last_overlays_.clear();
      }
    } else if (pending_video_.has_video_frame() && last_overlays_.size() < 16) {
      overlays = last_overlays_;
    }
    // pending_video_ is written by the command processor, so reading it needs
    // the mutex - but only for the read, not for what is built from it.
    std::lock_guard video_lock(snapshot_mutex_);
    if (!saw_video && no_video_presents_ > 45 && overlays.size() >= 8) {
      pending_video_.clear_video_frame();
      last_video_key_.store(0, std::memory_order_relaxed);
    }
    // With targets followed the frame goes where the title drew it - into
    // its own target, which it then copies out for the menu to draw. Put in
    // front of everything it was wiped by that target's clear, and the menu
    // drew a black copy.
    bool video_placed = false;
    for (auto it = overlays.begin(); it != overlays.end();) {
      if (!it->video_slot) {
        ++it;
        continue;
      }
      // Every slot: a present can hold two of the title's frames, each
      // drawing the video and copying it out, and a slot left empty made that
      // copy black - a black flash.
      if (pending_video_.has_video_frame()) {
        *it = pending_video_;
        video_placed = true;
        ++it;
      } else {
        it = overlays.erase(it);
      }
    }
    // Without a place of its own this frame the video is not drawn at all
    // there: the stale frame put in front of the garage made it flicker.
    static const bool targets_followed = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
    bool has_3d = false;
    for (const auto& ov : overlays) {
      if (ov.d3d_vertex_buffer != 0 && ov.num_indices > 100) {
        has_3d = true;
        break;
      }
    }
    if (!video_placed && (!targets_followed || !has_3d) && pending_video_.has_video_frame()) {
      batch.push_back(pending_video_);
    }
    if (batch.empty()) {
      batch.swap(overlays);
    } else {
      batch.insert(batch.end(), std::make_move_iterator(overlays.begin()),
                   std::make_move_iterator(overlays.end()));
    }
    InterpolateFrame(batch);
    if (batch.empty()) {
      bool has_resolves = false;
      {
        std::lock_guard res_lock(resolve_mutex_);
        has_resolves = !pending_resolves_.empty();
      }
      if (presented_once_ && !has_resolves) {
        return;
      }
    }
  }

  presented_once_ = true;

  struct EncodeCtx {
    PlumeDrawContext* draw = nullptr;
    memory::Memory* memory = nullptr;
    const std::vector<GuestDrawSnapshot>* draws = nullptr;
    PlumeGraphicsSystem* system = nullptr;
  } ctx{draw_context_.get(), memory_, &batch, this};

  auto encode = [](void* raw, plume::RenderCommandList* list, uint32_t width, uint32_t height,
                   plume::RenderTexture* color, const RenderPassBreak& pass) {
    auto* encode_ctx = static_cast<EncodeCtx*>(raw);
    if (!encode_ctx->draw || !encode_ctx->draws) {
      return;
    }
    // Split the frame's cost. The whole present runs up to two seconds in a
    // scene; encoding the draws and waiting on the swapchain are quite
    // different problems, and the totals say which one it is.
    const auto encode_started = std::chrono::steady_clock::now();
    struct EncodeTimer {
      std::chrono::steady_clock::time_point started;
      ~EncodeTimer() {
        static std::atomic<uint64_t> total_us{0};
        static std::atomic<uint64_t> frames{0};
        static std::atomic<uint64_t> last_ms{0};
        const uint64_t took = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        total_us.fetch_add(took, std::memory_order_relaxed);
        g_present_encode_us = took;
        const uint64_t n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        uint64_t was = last_ms.load(std::memory_order_relaxed);
        if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
          const uint64_t total = total_us.load(std::memory_order_relaxed);
          REXLOG_INFO("plume: encoding {} frame(s), {} ms total, {} us each", n, total / 1000,
                      total / (n ? n : 1));
        }
      }
    } encode_timer{encode_started};
    encode_ctx->draw->EncodeDraws(list, *encode_ctx->draws, encode_ctx->memory, width, height,
                                  color, &pass);
  };

  // Separate from encoding on purpose: this runs once the render pass is
  // closed. Copying out of the target while it is still bound for writing is
  // invalid, and doing it anyway corrupted the frame.
  auto resolve = [](void* raw, plume::RenderCommandList* list, uint32_t width, uint32_t height,
                    plume::RenderTexture* color, const RenderPassBreak&) {
    auto* encode_ctx = static_cast<EncodeCtx*>(raw);
    if (!encode_ctx->draw || !encode_ctx->system) {
      return;
    }
    for (const PendingResolve& request : encode_ctx->system->TakePendingResolves()) {
      // The guest's own extent when it gave one; the frame's otherwise.
      const uint32_t dest_width = request.dest_width ? request.dest_width : width;
      const uint32_t dest_height = request.dest_height ? request.dest_height : height;
      encode_ctx->draw->ResolveRenderTarget(list, color, width, height, request.dest_base,
                                            dest_width, dest_height);
    }
    // Show the front buffer, as the console does: the render target itself was
    // cleared for the next frame right after being resolved into it.
    encode_ctx->draw->PresentResolvedFrame(
        list, color, width, height, encode_ctx->system->swap_frontbuffer_.load(std::memory_order_relaxed));
  };

  const auto present_prepared = std::chrono::steady_clock::now();
  // A frame of nothing but a clear (and maybe a video frame) among full ones:
  // the last picture again, rather than a blink of the bare background.
  const bool hold = swapchain_->CanHoldFrames() && draw_context_ &&
                    draw_context_->HoldsThinFrame(batch);
  swapchain_->ClearAndPresent(0.03f, 0.04f, 0.07f, 1.0f, encode, &ctx, resolve, hold);
  // A slow frame, split: waiting for this renderer's lock, taking the queue,
  // encoding the draws (textures included), and the rest of the swapchain's
  // work - acquire, submit, present, fences.
  const auto present_done = std::chrono::steady_clock::now();
  const auto us = [](auto from, auto to) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
  };
  const uint64_t total_us = us(present_started, present_done);
  // The same split for every frame, averaged every two seconds: on a laptop
  // the present thread was busy the whole frame while the swap chain's own
  // part was a third of it, and only the slow frames said where the rest went.
  {
    static uint64_t lock_sum = 0, queue_sum = 0, encode_sum = 0, swap_sum = 0, frames = 0;
    static auto last_report = present_done;
    const uint64_t swap_us = us(present_prepared, present_done);
    lock_sum += us(present_started, present_locked);
    queue_sum += us(present_locked, present_prepared);
    encode_sum += g_present_encode_us;
    swap_sum += swap_us > g_present_encode_us ? swap_us - g_present_encode_us : 0;
    ++frames;
    if (present_done - last_report >= std::chrono::seconds(2)) {
      REXLOG_INFO("plume: present split (avg over {}): lock {} us, queue {} us, encode {} us, "
                  "swap chain {} us, {} draws last",
                  frames, lock_sum / frames, queue_sum / frames, encode_sum / frames,
                  swap_sum / frames, batch.size());
      lock_sum = queue_sum = encode_sum = swap_sum = frames = 0;
      last_report = present_done;
    }
  }
  if (total_us >= 100000) {
    const uint64_t swap_us = us(present_prepared, present_done);
    REXLOG_WARN("plume: slow present {} ms = lock {} ms, queue {} ms, encode {} ms, "
                "swapchain rest {} ms ({} draws)",
                total_us / 1000, us(present_started, present_locked) / 1000,
                us(present_locked, present_prepared) / 1000, g_present_encode_us / 1000,
                (swap_us > g_present_encode_us ? swap_us - g_present_encode_us : 0) / 1000,
                batch.size());
  }
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
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        // XERENGE_VBLANK_HZ: vblank interrupts at another rate than the video
        // mode the title is told about - to find out what its clock follows.
        if (const char* hz = std::getenv("XERENGE_VBLANK_HZ")) {
          refresh_rate_hz = std::max(1.0, std::strtod(hz, nullptr));
          REXLOG_INFO("plume: vblank at {} Hz", refresh_rate_hz);
        }
        const uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        const uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
        tl_vsync_worker = true;

        while (vsync_worker_running_) {
          // Consume the ring every tick so CP completion interrupts are not
          // delayed until the next vblank. Movie playback waits on those.
          PumpCommandProcessor();
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

void PlumeGraphicsSystem::PumpCommandProcessor() {
  uint32_t cp_mask = 0;
  {
    std::lock_guard lock(ring_mutex_);
    ConsumeRingBuffer();
    cp_mask = pending_cp_cpu_mask_;
    pending_cp_cpu_mask_ = 0;
  }
  // Dispatch after releasing the ring lock: the guest interrupt handler writes
  // GPU MMIO, and doing that while holding ring_mutex_ deadlocks.
  DispatchCpInterrupts(cp_mask);
}

void PlumeGraphicsSystem::DispatchCpInterrupts(uint32_t cpu_mask) {
  if (cpu_mask == 0) {
    return;
  }
  for (int n = 0; n < 6; ++n) {
    if (cpu_mask & (1u << n)) {
      DispatchInterruptCallback(1, n);
    }
  }
}

void PlumeGraphicsSystem::MarkVblank() {
  // The counter a fence publishes when its packet asks for the command
  // processor's own progress rather than a carried value. The reference
  // backend advances it here as well as on swap, so it climbs at the refresh
  // rate whatever the title is doing; ours advanced only when the title
  // presented, which ties it to the very thing a title waiting on this counter
  // is trying to get to. That is the xenos backend's behaviour, and it works
  // where this one stops.
  frame_counter_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(vblank_mutex_);
    vblank_count_.fetch_add(1, std::memory_order_release);
  }
  vblank_cv_.notify_all();

  // Progress was only ever published while consuming the ring, which is driven
  // by the title writing its write pointer. Once it blocks waiting for that
  // progress it stops writing, so the one thing that would release it stopped
  // happening for the same reason it was needed - a 24-byte shortfall that
  // nothing could ever close. Publishing here as well breaks that circle: it
  // reports the same drained-ring state, just on a clock of our own.
  {
    std::lock_guard lock(ring_mutex_);
    PublishGuestGpuProgress();
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

  // A guest burning CPU while the ring stays level is polling something. If it
  // polls through here, the register it is stuck on shows up as a read count
  // far beyond anything ordinary, and naming it is most of the diagnosis.
  {
    static std::mutex poll_mutex;
    static std::unordered_map<uint32_t, uint64_t> poll_counts;
    static std::set<uint32_t> poll_reported;
    std::lock_guard lock(poll_mutex);
    const uint64_t n = ++poll_counts[r];
    if (n == 2000000 && poll_reported.size() < 8 && poll_reported.insert(r).second) {
      REXLOG_WARN("plume: GPU register {:04X} read {} times - the guest looks stuck polling it",
                  r, n);
    }
  }

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x01C4: {  // CP_RB_RPTR
      // This used to answer with the write pointer - "everything consumed" -
      // which is not something the reference backend does, and it is the
      // register the title reads to decide how much room the ring has. The
      // truthful answer is the read pointer; xenos gives that and renders the
      // screens this one does not.
      static const bool pretend = std::getenv("XERENGE_RPTR_PRETEND") != nullptr;
      std::lock_guard lock(ring_mutex_);
      return pretend ? write_ptr_index_ : read_ptr_index_;
    }
    // CP_STAT and RBBM_STATUS2 answered zero - "idle, nothing outstanding" -
    // which the reference backend does not do either. Both describe how busy
    // the command processor is, and a title that believes it is idle behaves
    // differently from one that reads the register file like everything else.
    case 0x047F:
    case 0x039C: {
      static const bool pretend_idle = std::getenv("XERENGE_STATUS_IDLE") != nullptr;
      if (pretend_idle) {
        return 0;
      }
      break;
    }
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
  // Like present, this runs on a guest thread - the one that just rang the
  // doorbell - so every microsecond spent walking packets is a microsecond the
  // title is not running, and a second title thread may be spinning on a fence
  // this walk has yet to publish.
  const auto consume_started = std::chrono::steady_clock::now();
  struct ConsumeTimer {
    std::chrono::steady_clock::time_point started;
    std::atomic<uint32_t>* counter;
    std::atomic<uint64_t>* pull_ns;
    std::atomic<uint64_t>* copy_ns;
    std::atomic<uint64_t>* counter_draws;
    std::atomic<uint64_t>* fill_ns;
    std::atomic<uint64_t>* classify_ns;
    std::atomic<uint64_t>* capture_ns;
    std::atomic<uint64_t>* captures;
    std::atomic<uint64_t>* distinct;
    ~ConsumeTimer() {
      const auto took = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
      // The totals, not just the slow ones. A single 587 ms walk says nothing
      // about whether that is one bad frame or every frame, and the difference
      // decides whether this needs moving off the guest thread or simply
      // fixing where it is.
      {
        static std::atomic<uint64_t> total_us{0};
        static std::atomic<uint64_t> walks{0};
        static std::atomic<uint64_t> slowest_us{0};
        static std::atomic<uint64_t> last_ms{0};
        total_us.fetch_add(static_cast<uint64_t>(took), std::memory_order_relaxed);
        const uint64_t n = walks.fetch_add(1, std::memory_order_relaxed) + 1;
        uint64_t was_slowest = slowest_us.load(std::memory_order_relaxed);
        while (static_cast<uint64_t>(took) > was_slowest &&
               !slowest_us.compare_exchange_weak(was_slowest, static_cast<uint64_t>(took))) {
        }
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        uint64_t was = last_ms.load(std::memory_order_relaxed);
        if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
          const uint64_t total = total_us.load(std::memory_order_relaxed);
          REXLOG_INFO("plume: ring walks {}, {} ms total, {} us each, slowest {} us", n,
                      total / 1000, total / (n ? n : 1),
                      slowest_us.load(std::memory_order_relaxed));
        }
      }
      if (took < 8000) {
        return;
      }
      const uint32_t n = counter->fetch_add(1);
      if (n < 8 || (n % 120) == 0) {
        const uint64_t draws = counter_draws->load(std::memory_order_relaxed);
      REXLOG_WARN(
          "plume: ring walk took {} us on the guest thread; {} draws so far cost {} ms pulling "
          "constants and {} ms copying them",
          took, draws, pull_ns->load(std::memory_order_relaxed) / 1000000,
          copy_ns->load(std::memory_order_relaxed) / 1000000);
      REXLOG_WARN("plume:   of which filling {} ms, classifying {} ms, capturing video {} ms",
                  fill_ns->load(std::memory_order_relaxed) / 1000000,
                  classify_ns->load(std::memory_order_relaxed) / 1000000,
                  capture_ns->load(std::memory_order_relaxed) / 1000000);
      REXLOG_WARN("plume:   {} video capture(s) for {} distinct frame(s)",
                  captures->load(std::memory_order_relaxed),
                  distinct->load(std::memory_order_relaxed));
      }
    }
  } consume_timer{consume_started, &slow_consume_log_count_, &snapshot_pull_ns_,
                  &snapshot_copy_ns_, &snapshot_draws_, &snapshot_fill_ns_,
                  &snapshot_classify_ns_, &snapshot_capture_ns_, &video_captures_,
                  &video_distinct_frames_};

  ExecutePrimaryRing();
  if (0x01C4 < gpu_registers_.size()) {
    gpu_registers_[0x01C4] = read_ptr_index_;
  }
  WriteReadPointerWriteback();
  PublishGuestGpuProgress();
}

// The title will not submit more work until it sees the GPU report that it has
// caught up, and it reads that report from an address of its own choosing -
// kept in its bookkeeping, separate from the read pointer write-back the
// kernel asks for. Nothing here ever wrote it, so from the title's side the
// GPU had stopped, and it waited for room that never came.
//
// What gets published is the title's own submitted count, and only once the
// ring has actually drained: the commands really have been carried out by
// then, so this reports finished work rather than asserting it.
void PlumeGraphicsSystem::PublishGuestGpuProgress() {
  // The reference backend publishes nothing into this block - both words are the
// title's own, written by the fence packets it puts in the ring - and that is the
// build where the scene renders. Ours grew these writes while two real defects
// (a dropped EVENT_WRITE and a read pointer written to the wrong address) made
// the block look stuck; with those fixed the writes are competing with the title
// for its own bookkeeping. Off unless asked for.
  static const bool disabled = std::getenv("XERENGE_NO_GPU_PROGRESS") != nullptr;
  // Say plainly which test turned a tick away. The title can sit short of a
  // value by a dozen counts while this runs sixty times a second, and without
  // this there is no way to tell "never called" from "called and declined".
  const auto decline = [&](const char* why) {
    const uint32_t n = progress_decline_log_count_.fetch_add(1);
    if (n < 8 || (n % 600) == 0) {
      REXLOG_INFO("plume: progress not published: {} (rptr {:08X} wptr {:08X})", why,
                  read_ptr_index_, write_ptr_index_);
    }
  };
  if (disabled || !memory_) {
    decline("no memory or turned off");
    return;
  }
  if (read_ptr_index_ != write_ptr_index_) {
    decline("ring not drained");
    return;
  }
  const uint32_t device = d3d_device_guest_.load(std::memory_order_relaxed);
  if (!device) {
    decline("no device address known");
    return;
  }
  if (!RangeReadable(memory_, device + 10396, sizeof(uint32_t))) {
    decline("device fields not readable");
    return;
  }
  auto* base = memory_->TranslateVirtual<uint8_t*>(device);
  if (!base) {
    return;
  }
  const uint32_t publish_at = rex::memory::load_and_swap<uint32_t>(base + 10384);
  const uint32_t submitted = rex::memory::load_and_swap<uint32_t>(base + 10396);
  if (!publish_at) {
    return;
  }
  // Translate exactly as the title does. Masking this down to a physical
  // address lands on different host memory than the window the title reads
  // through - a write there is simply lost, which is why publishing appeared
  // to have no effect while the value it read never budged.
  auto* host = memory_->TranslateVirtual<uint8_t*>(publish_at);
  if (!host) {
    return;
  }
  const uint32_t published = rex::memory::load_and_swap<uint32_t>(host);
  // The two words move independently. Returning here when the first one is
  // already up to date - which it usually is - skipped the second entirely,
  // so it was only ever published on the rare tick where both had moved.
  if (published != submitted) {
    rex::memory::store_and_swap<uint32_t>(host, submitted);
    // Capped at eight this only ever described the first second of a run, and
    // the question is always what it is doing minutes later.
    const uint32_t n = gpu_progress_log_count_.fetch_add(1);
    if (n < 8 || (n % 600) == 0) {
      REXLOG_INFO("plume: published GPU progress {:08X} -> {:08X} at {:08X}", published, submitted,
                  publish_at);
    }
  }
  // Read the word back after writing it. The title reports a shortfall of
  // exactly twelve in every run regardless of the absolute values, which is
  // too steady to be a race - either the write is not landing, or something
  // puts an older value back afterwards, and only the readback tells them
  // apart.
  {
    const uint32_t n = progress_state_log_count_.fetch_add(1);
    if ((n % 600) == 0) {
      const uint32_t after = rex::memory::load_and_swap<uint32_t>(host);
      REXLOG_INFO("plume: progress word now {:08X}, title has submitted {:08X} (short by {})",
                  after, submitted, submitted - after);
    }
  }
  PublishSecondaryPosition(publish_at);
}

// The second word of that block is the position in the command buffer the
// title's flow control blocks on, and it only ever blocks there to allocate
// space it considers already submitted. With the ring drained, everything
// submitted has been executed, so reporting that the command processor stands
// where the title has written to states what has actually happened.
//
// The position itself comes from the title's own field rather than anything
// derived here, and the low two bits - the segment tag it maintains - are left
// exactly as they were.
void PlumeGraphicsSystem::PublishSecondaryPosition(uint32_t block) {
  static const bool disabled = std::getenv("XERENGE_NO_SECONDARY_POSITION") != nullptr;
  if (disabled || !memory_) {
    return;
  }
  const uint32_t device = d3d_device_guest_.load(std::memory_order_relaxed);
  if (!device || !RangeReadable(memory_, device + 14016, sizeof(uint32_t))) {
    return;
  }
  auto* device_host = memory_->TranslateVirtual<uint8_t*>(device);
  auto* block_host = memory_->TranslateVirtual<uint8_t*>(block + 4);
  if (!device_host || !block_host) {
    return;
  }
  const uint32_t reserve = rex::memory::load_and_swap<uint32_t>(device_host + 14016);
  if (!reserve) {
    return;
  }
  // How long the position must sit still before we step in. Thirty ticks -
  // half a second - makes this a deadlock breaker, and the render thread then
  // advances twice a second, which looks exactly like a frozen picture beside
  // audio and input that still respond. One tick makes it a continuous report:
  // with the ring drained, the command processor really is where the title has
  // written to, so saying so every vblank is not a lie.
  static const uint32_t stuck_ticks = [] {
    const char* e = std::getenv("XERENGE_SECONDARY_TICKS");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : kSecondaryStuckTicks;
  }();
  // Only step in once the position has genuinely stopped moving. Published on
  // every tick, this says the command processor has already passed everything
  // the title has written - which frees the space it is waiting for, but also
  // tells it there is no accumulated segment left to submit, and it stops
  // issuing draws altogether. The packets in the stream are the honest source
  // for this word; this is here to break a deadlock, not to replace them.
  // The "has it stopped moving" test belongs to the deadlock-breaker only. A
  // running title moves its cursor on almost every tick, so with the test in
  // place a continuous report almost never fires - which is why the published
  // generation stayed stale at 1 while the title was already on lap 0x186, and
  // the gate could not match whatever we did publish.
  if (stuck_ticks > 1) {
    if (reserve != last_secondary_reserve_) {
      last_secondary_reserve_ = reserve;
      secondary_unchanged_ticks_ = 0;
      return;
    }
  }
  // How long the position must sit still before we step in. Thirty ticks -
  // half a second - makes this a deadlock breaker, and the render thread then
  // advances twice a second, which looks exactly like a frozen picture beside
  // audio and input that still respond. One tick makes it a continuous report:
  // with the ring drained, the command processor really is where the title has
  // written to, so saying so every vblank is not a lie.
  if (stuck_ticks > 1 && ++secondary_unchanged_ticks_ < stuck_ticks) {
    return;
  }
  secondary_unchanged_ticks_ = 0;
  // The low two bits are a wrap generation, and the waiter tests them first:
  // it compares (its target - published) & 3 and only looks at the address at
  // all when that difference is exactly one. Carrying the previous generation
  // forward left it stale across a wrap, and a stale generation produces a
  // difference of two or three - which never matches, whatever the address
  // says. The title keeps the current generation in the field next to the
  // position, so take it from there.
  const uint32_t current = rex::memory::load_and_swap<uint32_t>(block_host);
  const uint32_t generation = RangeReadable(memory_, device + 14020, sizeof(uint32_t))
                                  ? rex::memory::load_and_swap<uint32_t>(device_host + 14020)
                                  : current;
  const uint32_t reached = (reserve & ~uint32_t(0x3)) | (generation & 0x3u);
  if (reached == current) {
    return;
  }
  rex::memory::store_and_swap<uint32_t>(block_host, reached);
  const uint32_t n = secondary_log_count_.fetch_add(1);
  if (n < 8 || (n % 600) == 0) {
    REXLOG_INFO("plume: secondary position {:08X} -> {:08X}", current, reached);
  }
}

void PlumeGraphicsSystem::WriteReadPointerWriteback() {
  if (!read_ptr_writeback_ptr_ || !memory_) {
    return;
  }
  // The title reads this through the identifier block, at devblock+60, and
  // that block is addressed through the window whose physical spelling is a
  // page away from the obvious one. Writing the address the kernel hands over
  // as a physical one lands elsewhere: the slot the title actually reads sat
  // at zero for every run so far, while the primary ring wait
  // (sub_823819D0) compared its write pointer against that zero.
  //
  // So write it where the title looks, translated the way the title
  // translates it, and keep the kernel-supplied address as the fallback for
  // before the device is known.
  const uint32_t device = d3d_device_guest_.load(std::memory_order_relaxed);
  if (device != 0 && RangeReadable(memory_, device + 10384, sizeof(uint32_t))) {
    if (auto* device_host = memory_->TranslateVirtual<uint8_t*>(device)) {
      const uint32_t block = rex::memory::load_and_swap<uint32_t>(device_host + 10384);
      if (block != 0) {
        if (auto* slot = memory_->TranslateVirtual<uint8_t*>(block + 60)) {
          // The colleague's working build puts a monotonically increasing
          // per-frame counter here rather than the ring's read index. The
          // title's primary-ring space check reads this slot and spins while
          // the dwords it is about to overwrite still look unconsumed; a value
          // that only ever climbs never looks like an index inside that span,
          // so the check passes. Writing the true index is more honest and is
          // what we have been doing - and it is also the only backend that
          // stops here.
          static const bool as_counter = std::getenv("XERENGE_RPTR_COUNTER") != nullptr;
          const uint32_t published =
              as_counter ? frame_counter_.load(std::memory_order_relaxed) : read_ptr_index_;
          rex::memory::store_and_swap<uint32_t>(slot, published);
          const uint32_t n = rptr_log_count_.fetch_add(1);
          if (n < 4 || (n % 2000) == 0) {
            REXLOG_INFO("plume: read pointer {:08X} published at {:08X}", published,
                        block + 60);
          }
          return;
        }
      }
    }
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

uint32_t PlumeGraphicsSystem::Pm4ReadPhysical(uint32_t phys_addr) {
  if (!memory_) {
    return 0;
  }
  auto* host = memory_->TranslatePhysical<uint8_t*>(phys_addr & ~uint32_t(0x3));
  return host ? rex::memory::load_and_swap<uint32_t>(host) : 0u;
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

  // RB_COPY_* is how the title asks for a resolve: take what was rendered into
  // EDRAM and copy it out to a texture in guest memory. Nothing here does
  // that, so a title that then waits for the copy to appear waits forever.
  // Recording the request makes that visible instead of it looking like a
  // freeze with no cause.
  // RB_COPY_* is how the title asks for a resolve: take what was rendered and
  // copy it out into one of its own textures, which later draws then sample.
  // Writing the destination base is the last of the three, so that is where
  // the request is considered complete and queued for the present thread.
  // With the render targets taken from the Direct3D calls, every copy already
  // has its marker, laid down by the Resolve and EndTiling hooks at the point
  // in the draws where the title made it. The same copy read back out of the
  // ring arrives late - often after the target has been cleared for the next
  // pass - and copying then put a black picture into the front buffer, which
  // is what was shown whenever it won the race: the 3D scene flickered
  // through black. Swap's own copy of a 3D frame is the target as it stands at
  // the end of the frame, which is what gets shown without a copy.
  static const bool d3d_targets = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  if (index == 0x2319 && !d3d_targets) {
    std::lock_guard lock(resolve_mutex_);
    PendingResolve resolve;
    resolve.dest_base = value;
    resolve.copy_control = gpu_registers_[0x2318];
    resolve.dest_info = gpu_registers_[0x231B];
    // RB_COPY_DEST_PITCH carries the size the guest expects the copy to be,
    // which is its own render resolution and not the size of this host window.
    const uint32_t dest_pitch = gpu_registers_[0x231A];
    resolve.dest_width = dest_pitch & 0x3FFFu;
    resolve.dest_height = (dest_pitch >> 16) & 0x3FFFu;
    // Each copy shows up twice: once as the request, once as the guest
    // clearing the control register afterwards. Only the first is a request.
    if (resolve.dest_base != 0 && resolve.copy_control != 0) {
      pending_resolves_.push_back(resolve);
      // Put a marker where the request happened. Carrying every copy to the
      // end of the frame means copying the finished picture rather than the
      // scene the title wanted, and everything drawn afterwards from that
      // copy - which on these screens is the whole interface - then samples
      // the wrong thing.
      {
        GuestDrawSnapshot marker;
        marker.is_resolve = true;
        marker.resolve_flags = resolve.copy_control;
        marker.resolve_dest = resolve.dest_base;
        marker.resolve_width = resolve.dest_width;
        marker.resolve_height = resolve.dest_height;
        std::lock_guard lock(snapshot_mutex_);
        draw_ring_[draw_ring_next_] = std::move(marker);
        draw_ring_next_ = (draw_ring_next_ + 1) % kDrawRingSize;
        if (draw_ring_count_ < kDrawRingSize) {
          ++draw_ring_count_;
        }
      }
      static std::atomic<uint32_t> resolve_logs{0};
      const uint32_t n = resolve_logs.fetch_add(1);
      if (n < 8) {
        REXLOG_INFO("plume: resolve requested -> dest {:08X} (control {:08X}, info {:08X})",
                    resolve.dest_base, resolve.copy_control, resolve.dest_info);
      }
    }
  }
}

std::vector<PlumeGraphicsSystem::PendingResolve> PlumeGraphicsSystem::TakePendingResolves() {
  std::lock_guard lock(resolve_mutex_);
  std::vector<PendingResolve> out;
  out.swap(pending_resolves_);
  // A frame can ask for the same destination several times; only the last
  // state of it is what the guest goes on to sample.
  if (out.size() > 1) {
    std::vector<PendingResolve> unique;
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
      const bool seen = std::any_of(unique.begin(), unique.end(), [&](const PendingResolve& r) {
        return r.dest_base == it->dest_base;
      });
      if (!seen) {
        unique.push_back(*it);
      }
    }
    out.swap(unique);
  }
  return out;
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
  // Writing these is a step in the offline shader-translation pipeline, not
  // something a run needs - and it happens here, on the command processor
  // thread, the first time each shader is seen. Entering a scene loads
  // seventeen of them at once, so the walk stops for seventeen directory
  // creations, file existence checks and writes at exactly the moment the
  // title is switching what it draws. Off unless asked for.
  static const bool dumping = std::getenv("XERENGE_DUMP_UCODE") != nullptr;
  if (!dumping) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories("generated/ucode-dump", ec);
  char name[80];
  std::snprintf(name, sizeof(name), "%s_%016llX_%u.bin", shader_type == 0 ? "VS" : "PS",
                static_cast<unsigned long long>(microcode_hash), byte_size);
  const auto dir = std::filesystem::path("generated/ucode-dump");
  const auto path = dir / name;
  if (!std::filesystem::exists(path, ec)) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      return;
    }
    out.write(reinterpret_cast<const char*>(bytes), std::streamsize(byte_size));
  }

  // The declarations a container wrapper needs, recovered from the microcode.
  // Written for every shader, not just the ones that missed the cache: a
  // shader resolves today only because some earlier capture was wrapped by
  // hand, and the containers behind that were not kept. Describing everything
  // means the whole cache can be regenerated from a run, instead of new
  // shaders having to be bolted onto a cache nobody can rebuild.
  const bool is_vs = shader_type == 0;
  const std::string derived =
      is_vs ? PlumeDrawContext::DescribeVsWrapperElements(bytes, byte_size)
            : PlumeDrawContext::DescribePsWrapperInterpolators(bytes, byte_size);
  if (derived.empty()) {
    return;
  }
  char args_name[96];
  std::snprintf(args_name, sizeof(args_name), "%s_%016llX_%u.wrapargs", is_vs ? "VS" : "PS",
                static_cast<unsigned long long>(microcode_hash), byte_size);
  std::ofstream args(dir / args_name);
  if (args) {
    args << "# Arguments for the XenosRecomp container wrapper, derived from this\n"
         << (is_vs ? "# shader's own microcode: --element from its vertex fetch instructions\n"
                     "# (keyed by instruction address), --interpolator from its exports,\n"
                     "# --sampler from its texture fetches.\n"
                   : "# shader's own microcode: --interpolator from the temporaries it reads\n"
                     "# before writing, which is what the rasteriser hands it.\n")
         << derived.substr(1) << '\n';
  }
}

void PlumeGraphicsSystem::BindLoadedShader(uint32_t shader_type, uint64_t microcode_hash,
                                           uint32_t byte_size, const uint8_t* bytes) {
  // Entering a scene loads seventeen shaders back to back and everything stops
  // there, in every run. Time the handling itself so "it is slow here" can be
  // told apart from "it never returns from here".
  const auto bind_started = std::chrono::steady_clock::now();
  struct BindTimer {
    std::chrono::steady_clock::time_point started;
    uint64_t hash;
    ~BindTimer() {
      const auto took = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
      if (took >= 2000) {
        REXLOG_WARN("plume: binding shader {:016X} took {} us", hash, took);
      }
    }
  } bind_timer{bind_started, microcode_hash};
  if (microcode_hash == 0) {
    return;
  }

  PlumeShaderCache& cache = PlumeShaderCache::Instance();
  const ShaderMicrocodeEntry* micro = cache.FindByMicrocode(microcode_hash);
  const uint64_t shader_hash = micro ? micro->shaderHash : 0;
  if (shader_type == 0) {
    active_vs_hash_ = shader_hash ? shader_hash : microcode_hash;
  } else {
    active_ps_hash_ = shader_hash ? shader_hash : microcode_hash;
  }

  // With the Direct3D path on, the title's own shader loads say exactly which
  // object is which shader (NoteShaderLoad). Pairing ring loads with bound
  // objects in order was only ever a guess, and it kept running alongside and
  // overwrote the exact answers: object addresses are reused, the queue held
  // one that had since become a pixel shader, and a vertex shader was handed
  // to the fragment stage - which the driver's compiler hung on.
  static const bool d3d_loads_authoritative = std::getenv("XERENGE_D3D_DRAWS") != nullptr;
  if (!d3d_loads_authoritative) {
    std::lock_guard lock(guest_shader_mutex_);
    const uint32_t stage = shader_type == 0 ? 0u : 1u;
    auto& queue = pending_shader_objects_[stage];
    if (!queue.empty()) {
      const uint32_t object = queue.front();
      queue.pop_front();
      const uint64_t resolved = shader_hash ? shader_hash : microcode_hash;
      guest_shaders_[object] = GuestShaderSlot{resolved, shader_type != 0};
      const uint32_t n = shader_object_log_count_.fetch_add(1);
      if (n < 8) {
        REXLOG_INFO("plume: shader object {:08X} is {} {:016X}", object,
                    shader_type == 0 ? "VS" : "PS", resolved);
      }
    }
  }
  const bool first_seen = seen_microcode_hashes_.insert(microcode_hash).second;
  // Only the first time this microcode appears. This runs on the command
  // processor thread and touches the filesystem, and a shader is re-bound
  // constantly - doing it per bind stalls the title outright.
  if (first_seen && bytes) {
    DumpMicrocode(shader_type, microcode_hash, bytes, byte_size);
  }
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
    // DumpMicrocode has already written this shader's microcode and the
    // container declarations recovered from it, so translating it is a matter
    // of running tools/translate_runtime_shaders.sh.
  }
}

void PlumeGraphicsSystem::ExecutePrimaryRing() {
  if (!memory_ || ring_buffer_ptr_ == 0 || ring_buffer_size_ < 4) {
    read_ptr_index_ = write_ptr_index_;
    return;
  }
  const uint32_t dword_count = ring_buffer_size_ / 4;
  const uint32_t wrap_mask = dword_count - 1;

  // A stall shows up as the title going quiet, which says nothing about who
  // stopped. These two pointers do: the guest advances the write pointer as it
  // submits, this thread advances the read pointer as it consumes. If they are
  // level the guest has stopped submitting and is waiting on something; if the
  // write pointer has run ahead, the fault is on this side.
  {
    static auto last_change = std::chrono::steady_clock::now();
    static uint32_t last_wptr = 0;
    static bool reported = false;
    const auto now = std::chrono::steady_clock::now();
    if (write_ptr_index_ != last_wptr) {
      last_wptr = write_ptr_index_;
      last_change = now;
      reported = false;
    } else if (!reported && now - last_change > std::chrono::seconds(3)) {
      reported = true;
      REXLOG_WARN(
          "plume: no ring activity for 3s - rptr {:08X} wptr {:08X} ({}); "
          "guest {} submitting",
          read_ptr_index_, write_ptr_index_,
          read_ptr_index_ == write_ptr_index_ ? "level" : "work pending",
          read_ptr_index_ == write_ptr_index_ ? "has stopped" : "is still");
      DumpRecentGpuEvents();
      // What the title can actually see right now at the places the GPU is
      // expected to update. If one of these is the value it is waiting on,
      // comparing it against what was last written says whether the write
      // landed, and where.
      // Those offsets - past ten thousand bytes - belong to the guest D3D
      // device, which this backend already tracks for its constants. So the
      // structure needs no searching: read the fields straight out of it.
      if (memory_) {
        const uint32_t device = d3d_device_guest_.load(std::memory_order_relaxed);
        // TranslateVirtual does not check that an address is mapped - it just
        // offsets the base - so every read has to be guarded first, or an
        // unmapped one takes the process down.
        auto read = [&](uint32_t at) -> uint32_t {
          if (!RangeReadable(memory_, at, sizeof(uint32_t))) {
            return 0u;
          }
          auto* host = memory_->TranslateVirtual<uint8_t*>(at);
          return host ? rex::memory::load_and_swap<uint32_t>(host) : 0u;
        };
        const uint32_t device_for_read = device;
        if (device_for_read != 0) {
          const uint32_t gpu_ptr = read(device_for_read + 10384);
          const uint32_t write_ptr = read(device_for_read + 10396);
          const uint32_t gpu_value = gpu_ptr != 0 ? read(gpu_ptr) : 0u;
          REXLOG_WARN(
              "plume: D3D device {:08X}: it reads progress through {:08X} (holding {:08X}), "
              "its write pointer is {:08X}, difference {:08X}",
              device_for_read, gpu_ptr, gpu_value, write_ptr, write_ptr - gpu_value);
        } else {
          REXLOG_WARN("plume: no D3D device address known, cannot read its ring fields");
        }
      }

      // The title tracks its ring with a structure holding a pointer to the
      // fence slot at +10384 and its own write pointer at +10396. The
      // structure's address is only known to the guest, but it can be found:
      // scan for a word equal to a fence address and read the write pointer
      // twelve bytes further on. That gives both sides of the comparison the
      // title is stuck on, which its own registers would have shown had the
      // debugger been able to see them.
      // Half a second of the title's own thread (every word of 512 MB), paid on
      // the first load of every run - opt in with XERENGE_RING_STALL_SCAN=1.
      static const bool scan_enabled = std::getenv("XERENGE_RING_STALL_SCAN") != nullptr;
      if (scan_enabled && memory_ && g_fence_count != 0) {
        static bool scanned = false;
        if (!scanned) {
          scanned = true;
          constexpr uint32_t kScanEnd = 0x20000000u;
          constexpr uint32_t kPtrOffset = 10384u;
          constexpr uint32_t kWriteOffset = 10396u;
          uint32_t found = 0;
          for (uint32_t at = 0x1000; at + 4 <= kScanEnd && found < 16; at += 4) {
            auto* host = memory_->TranslatePhysical<uint8_t*>(at);
            if (!host) {
              continue;
            }
            const uint32_t word = rex::memory::load_and_swap<uint32_t>(host);
            bool is_fence = false;
            for (size_t i = 0; i < g_fence_count; ++i) {
              // Register space is not a fence the title polls through memory;
              // its value turns up all over the heap and only yields false
              // matches.
              if (g_fence_addresses[i] >= 0xC0000000u) {
                continue;
              }
              // A packet carries a physical address, but the title keeps its
              // pointers virtual - the same bytes reached through one of the
              // mapped windows. Searching only for the physical spelling is
              // why the first scan found nothing.
              const uint32_t phys = g_fence_addresses[i];
              is_fence = is_fence || word == phys || word == (phys | 0x80000000u) ||
                         word == (phys | 0xA0000000u) || word == (phys | 0xC0000000u) ||
                         word == (phys | 0xE0000000u);
            }
            if (!is_fence || at < kPtrOffset) {
              continue;
            }
            const uint32_t base = at - kPtrOffset;
            auto* wp = memory_->TranslatePhysical<uint8_t*>(base + kWriteOffset);
            if (!wp) {
              continue;
            }
            const uint32_t write_ptr = rex::memory::load_and_swap<uint32_t>(wp);
            ++found;
            REXLOG_WARN(
                "plume: ring bookkeeping at {:08X}: fence slot {:08X} holds {:08X}, "
                "write pointer {:08X}, so {} bytes look outstanding",
                base, word,
                [&] {
                  auto* f = memory_->TranslatePhysical<uint8_t*>(word);
                  return f ? rex::memory::load_and_swap<uint32_t>(f) : 0u;
                }(),
                write_ptr, write_ptr - [&] {
                  auto* f = memory_->TranslatePhysical<uint8_t*>(word);
                  return f ? rex::memory::load_and_swap<uint32_t>(f) : 0u;
                }());
          }
          if (found == 0) {
            REXLOG_WARN("plume: no ring bookkeeping found pointing at a fence slot");
          }
        }
      }
      if (memory_) {
        for (size_t i = 0; i < g_fence_count; ++i) {
          const uint32_t at = g_fence_addresses[i];
          auto* host = memory_->TranslatePhysical<uint8_t*>(at);
          REXLOG_WARN("plume: fence slot {:08X} currently holds {:08X}", at,
                      host ? rex::memory::load_and_swap<uint32_t>(host) : 0u);
          // A little of the surrounding memory too - a ring's read and write
          // pointers usually live next to each other.
          for (uint32_t off = 0; off < 32; off += 8) {
            auto* a = memory_->TranslatePhysical<uint8_t*>(at - 16 + off);
            auto* b = memory_->TranslatePhysical<uint8_t*>(at - 16 + off + 4);
            if (a && b) {
              REXLOG_WARN("plume:   {:08X}: {:08X} {:08X}", at - 16 + off,
                          rex::memory::load_and_swap<uint32_t>(a),
                          rex::memory::load_and_swap<uint32_t>(b));
            }
          }
        }
      }
    }
  }

  if (!ExecutePm4Buffer(ring_buffer_ptr_, read_ptr_index_, write_ptr_index_, wrap_mask, true, 0)) {
    REXLOG_WARN("plume: PM4 primary walk failed; snapping rptr {:08X} -> {:08X}", read_ptr_index_,
                write_ptr_index_);
  }
  read_ptr_index_ = write_ptr_index_;

  // Publish the read pointer where the guest looks for it. CP_RB_RPTR_ADDR
  // names a location in guest memory that the command processor is expected to
  // keep up to date; the title reads it - not the register - to work out how
  // much room is left in the ring, and waits when it thinks there is none.
  // Never writing it leaves that value frozen at whatever it started as, so
  // once the ring has been filled around once the title waits for space that,
  // as far as it can see, never comes back.
  const uint32_t writeback_raw = gpu_registers_[0x01C3];
  {
    // Report the raw register and what is currently at the address it names,
    // before touching anything. Writing the read pointer there is what the
    // title waits on, but the register's units are not obvious - guessing at
    // them and writing regardless took the interface out entirely - so the
    // value and its target are measured first and the write stays opt-in.
    static uint32_t announced = 0;
    if (announced < 4 && writeback_raw != 0 && memory_) {
      ++announced;
      const uint32_t as_is = writeback_raw & ~uint32_t(0x3);
      const uint32_t as_dwords = (writeback_raw << 2) & ~uint32_t(0x3);
      auto* at_as_is = memory_->TranslatePhysical<uint8_t*>(as_is & 0x1FFFFFFFu);
      auto* at_dwords = memory_->TranslatePhysical<uint8_t*>(as_dwords & 0x1FFFFFFFu);
      REXLOG_INFO(
          "plume: CP_RB_RPTR_ADDR = {:08X}; as an address it holds {:08X}, as a dword "
          "index it points at {:08X} holding {:08X}; our rptr is {:08X}",
          writeback_raw, at_as_is ? rex::memory::load_and_swap<uint32_t>(at_as_is) : 0u,
          as_dwords, at_dwords ? rex::memory::load_and_swap<uint32_t>(at_dwords) : 0u,
          read_ptr_index_);
    }
  }
  static const bool publish_rptr = std::getenv("XERENGE_PUBLISH_RPTR") != nullptr;
  if (publish_rptr && writeback_raw != 0 && memory_) {
    if (auto* host =
            memory_->TranslatePhysical<uint8_t*>((writeback_raw & 0x1FFFFFFFu) & ~uint32_t(0x3))) {
      rex::memory::store_and_swap<uint32_t>(host, read_ptr_index_);
    }
  }
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
  using rex::graphics::xenos::PM4_XE_SWAP;
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

    // The same journal the reference backend writes, in the same format, so
    // the two can be compared line for line on one route. The first line where
    // they differ is where this backend stops seeing what the other sees.
    {
      static std::FILE* trace = [] () -> std::FILE* {
        const char* path = std::getenv("XERENGE_PM4_TRACE");
        return path ? std::fopen(path, "w") : nullptr;
      }();
      if (trace) {
        std::fprintf(trace, "%02X %u\n", opcode, count);
      }
    }

    // Tiled rendering. The title renders the scene by replaying one command
    // stream once per tile of the frame buffer, and marks the packets that
    // belong to a particular tile by setting bit 0 of their header. Which
    // tiles are live is programmed through BIN_MASK and BIN_SELECT; a marked
    // packet runs only while those two still share a bit.
    //
    // Both start as all-ones so an unprogrammed stream executes everything,
    // which is why menus - drawn without tiling - were never affected by this
    // being missing.
    static const bool predication_off = std::getenv("XERENGE_NO_PREDICATION") != nullptr;
    if (!predication_off && (packet & 0x1u) != 0 && (bin_select_ & bin_mask_) == 0) {
      if (!skip_words(count)) {
        return false;
      }
      ++predicated_skips_;
      continue;
    }

    // The two halves arrive high word first, and each has its own packet for
    // writing one half alone.
    if (opcode == 0x50 || opcode == 0x51) {
      // A short form of this packet carries one word, not two. Treating that
      // as an error abandoned the entire walk from that point on - and since
      // the title emits these hundreds of thousands of times while replaying
      // the scene per tile, everything after the third one was simply never
      // read. The reference backend executes 462732 of these where this one
      // saw two.
      uint32_t hi = 0;
      uint32_t lo = 0;
      if (count < 2) {
        if (!skip_words(count)) {
          return false;
        }
        continue;
      }
      if (!read_word(hi) || !read_word(lo) || !skip_words(count - 2)) {
        return false;
      }
      const uint64_t value = (uint64_t(hi) << 32) | lo;
      (opcode == 0x50 ? bin_mask_ : bin_select_) = value;
      const uint32_t n = bin_log_count_.fetch_add(1);
      if (n < 32 || (n % 600) == 0) {
        REXLOG_INFO("plume: tiling state: mask {:016X} select {:016X}, {} packet(s) skipped so far",
                    bin_mask_, bin_select_, predicated_skips_);
      }
      continue;
    }
    if (opcode >= 0x60 && opcode <= 0x63) {
      uint32_t half = 0;
      if (count < 1) {
        continue;
      }
      if (!read_word(half) || !skip_words(count - 1)) {
        return false;
      }
      uint64_t& target = (opcode <= 0x61) ? bin_mask_ : bin_select_;
      const bool high = (opcode & 0x1u) != 0;
      target = high ? ((target & 0xFFFFFFFFull) | (uint64_t(half) << 32))
                    : ((target & 0xFFFFFFFF00000000ull) | half);
      continue;
    }
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
        {
          uint32_t was = ib_longest_.load(std::memory_order_relaxed);
          while (list_length > was &&
                 !ib_longest_.compare_exchange_weak(was, list_length)) {
          }
        }
        if (list_length != 0) {
          // The title kicks 150 times a second and each kick hands over a
          // buffer, so these two counts should track each other. If they do
          // not, submissions are going somewhere this walk never looks - which
          // would leave the fences inside them unexecuted and the position the
          // title waits on frozen exactly as observed.
          {
            static std::atomic<uint64_t> executed{0};
            static std::atomic<uint64_t> dwords_total{0};
            static std::atomic<uint64_t> last_report_ms{0};
            const uint64_t count = executed.fetch_add(1, std::memory_order_relaxed) + 1;
            dwords_total.fetch_add(list_length, std::memory_order_relaxed);
            const uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            uint64_t last = last_report_ms.load(std::memory_order_relaxed);
            if (now_ms - last >= 1000 && last_report_ms.compare_exchange_strong(last, now_ms)) {
              // What the buffers actually carry. A second of submissions with
              // no draws in them says the title is issuing state and fences
              // and nothing to render, which is a different problem from the
              // draws being dropped on our side.
              std::string census;
              for (size_t i = 0; i < pm4_opcode_counts_.size(); ++i) {
                if (pm4_opcode_counts_[i] != 0) {
                  census += fmt::format(" {:02X}={}", i, pm4_opcode_counts_[i]);
                }
              }
              REXLOG_INFO(
                  "plume: {} indirect buffer(s), {} dwords, longest {}, {} aborted; "
                  "packets so far:{}",
                  count, dwords_total.load(std::memory_order_relaxed),
                  ib_longest_.load(std::memory_order_relaxed),
                  ib_failures_.load(std::memory_order_relaxed), census);
            }
          }
          if (n < 32) {
            REXLOG_INFO("plume: PM4 IB {:08X} dwords={}", list_ptr, list_length);
          }
          // An abort inside a nested buffer used to abort the walk around it
          // too, so everything after it in the outer buffer was lost. Count
          // them and say where, because losing the tail of a submission is
          // indistinguishable from the title never having sent it.
          if (!ExecutePm4Buffer(list_ptr, 0, list_length, 0, false, depth + 1)) {
            const uint64_t failed = ib_failures_.fetch_add(1) + 1;
            if (failed <= 8 || (failed % 5000) == 0) {
              REXLOG_WARN("plume: PM4 IB {:08X} dwords={} aborted (failure #{}, depth {})",
                          list_ptr, list_length, failed, depth);
            }
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
          NoteGpuEvent("MEM_WRITE", write_addr, write_data);
          Pm4WritePhysical(write_addr, write_data);
          write_addr += 4;
        }
        break;
      }
      // EVENT_WRITE carries the same fence payload as EVENT_WRITE_SHD and was
      // being skipped. It is the packet that publishes the wrap generation the
      // title's flow control waits on, and it is rare by design - so skipping
      // it cost one write every few hundred frames, which is exactly the one
      // that never arrived. Its short form carries only an event type.
      case 0x5A: {  // EVENT_WRITE_EXT
        // Writes a fixed set of screen extents to memory. The title reads them
        // back, so leaving the memory untouched leaves it reading whatever was
        // there before.
        uint32_t initiator = 0;
        uint32_t address = 0;
        if (count < 2 || !read_word(initiator) || !read_word(address)) {
          return false;
        }
        if (!skip_words(count - 2)) {
          return false;
        }
        Pm4StoreRegister(0x21F9, initiator & 0x3F);  // VGT_EVENT_INITIATOR
        const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
        if (memory_) {
          if (auto* host = memory_->TranslatePhysical<uint8_t*>(address & ~uint32_t(0x3))) {
            for (uint32_t i = 0; i < 6; ++i) {
              const uint16_t v = extents[i];
              host[i * 2 + 0] = static_cast<uint8_t>(v >> 8);
              host[i * 2 + 1] = static_cast<uint8_t>(v & 0xFF);
            }
          }
        }
        break;
      }
      case PM4_XE_SWAP: {
        // The frame is complete. Presenting from here rather than from VdSwap
        // puts the swap where the rest of the stream is, so the title's own
        // pacing and the picture agree.
        uint32_t magic = 0;
        uint32_t frontbuffer = 0;
        uint32_t swap_width = 0;
        uint32_t swap_height = 0;
        if (count < 4 || !read_word(magic) || !read_word(frontbuffer) ||
            !read_word(swap_width) || !read_word(swap_height)) {
          return false;
        }
        if (!skip_words(count - 4)) {
          return false;
        }
        NoteGpuEvent("XE_SWAP", frontbuffer, swap_width);
        // The front buffer this frame is shown from - the resolve into this
        // address is the picture, whatever other copies the frame makes.
        swap_frontbuffer_.store(frontbuffer & ~uint32_t(0xFFF), std::memory_order_relaxed);
        static const bool present_on_packet = std::getenv("XERENGE_PRESENT_ON_SWAP") != nullptr;
        if (present_on_packet) {
          PresentGuestFrame(swap_width, swap_height);
        }
        break;
      }
      case PM4_EVENT_WRITE:
      case PM4_EVENT_WRITE_SHD: {
        if (count < 3) {
          if (!skip_words(count)) {
            return false;
          }
          break;
        }
        uint32_t initiator = 0;
        uint32_t address = 0;
        uint32_t value = 0;
        if (!read_word(initiator) || !read_word(address) || !read_word(value)) {
          return false;
        }
        if (!skip_words(count - 3)) {
          return false;
        }
        // Bit 31 asks for the command processor's own counter rather than the
        // value carried by the packet. Writing a constant there - it used to
        // write 1 - means a title waiting for that counter to pass a mark
        // waits for something that never moves. The frame counter is the
        // monotonic quantity this backend has, and it advances on every guest
        // swap, which is the cadence such a counter is expected to keep.
        const uint32_t data_value = ((initiator >> 31) & 0x1)
                                        ? frame_counter_.load(std::memory_order_relaxed)
                                        : value;
        if (n < 32) {
          REXLOG_INFO("plume: PM4 EVENT_WRITE_SHD {:08X}={:08X}", address & ~uint32_t(0x3),
                      data_value);
        }
        NoteGpuEvent("EVENT_WRITE_SHD", address, data_value);
        NoteFenceAddress(address);
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
        const uint64_t microcode_hash = HashGuestPhysical(addr, byte_count);
        // Remember where this microcode lives. The title's shader objects
        // point at it, so a draw driven from the Direct3D calls can name its
        // shader by that pointer - which is stable - rather than by hashing
        // bytes whose length it has no way to know.
        if (addr != 0 && microcode_hash != 0 && byte_count != 0) {
          std::lock_guard lock(guest_shader_mutex_);
          auto& loads = microcode_at_address_[addr];
          const bool known = std::any_of(loads.begin(), loads.end(), [&](const MicrocodeLoad& l) {
            return l.byte_count == byte_count && l.hash == microcode_hash;
          });
          if (!known) {
            // Newest first: a reused buffer most often still holds its last
            // load, so the match is usually the first thing tried.
            loads.insert(loads.begin(), MicrocodeLoad{byte_count, microcode_hash});
            if (loads.size() > 8) {
              loads.pop_back();
            }
          }
        }
        BindLoadedShader(shader_type, microcode_hash, byte_count, host);
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
        ring_draw_opcode_ = opcode;
        PublishDrawSnapshot(prim_type, source_select, num_indices ? num_indices : 1);
        ring_draw_opcode_ = 0;
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
      case PM4_INTERRUPT: {
        uint32_t cpu_mask = 0;
        if (count < 1 || !read_word(cpu_mask)) {
          return false;
        }
        if (!skip_words(count - 1)) {
          return false;
        }
        pending_cp_cpu_mask_ |= cpu_mask;
        NoteGpuEvent("INTERRUPT cpu_mask", cpu_mask, 0);
        const uint32_t logged = interrupt_log_count_.fetch_add(1);
        // The first few, then a heartbeat. A video that hangs waiting on a CP
        // interrupt looks exactly like this line going quiet, so capping the
        // log at the first handful hid the only symptom worth seeing.
        if (logged < 8 || (logged % 256) == 0) {
          REXLOG_INFO("plume: PM4 INTERRUPT cpu_mask={:02X} (#{})", cpu_mask, logged + 1);
        }
        break;
      }
      case PM4_LOAD_CONSTANT_CONTEXT:
      case PM4_ME_INIT:
      case PM4_NOP:
      case PM4_WAIT_REG_MEM:
      case PM4_WAIT_FOR_IDLE:
      default: {
        // One line per opcode we do not implement. A guest that spins forever
        // is usually polling for something the command processor was supposed
        // to write, and a packet silently skipped here is the obvious place
        // for that to go missing - but only if it is visible.
        static std::mutex unhandled_mutex;
        static std::set<uint32_t> unhandled_seen;
        {
          std::lock_guard lock(unhandled_mutex);
          if (unhandled_seen.size() < 32 && unhandled_seen.insert(opcode).second) {
            REXLOG_INFO("plume: PM4 opcode {:02X} not implemented (count={}), skipped", opcode,
                        count);
          }
        }
        if (!skip_words(count)) {
          return false;
        }
        break;
      }
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
  StopPresentWorker();
  StopVideoWorker();
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
