/**
 * @file        plume_d3d_hooks.cpp
 * @brief       Guest D3D hooks for the plume backend (phase 3).
 */
#include <cmath>
#include "diagnostics.h"
#include "plume_d3d.h"

#include <atomic>
#include <set>
#include <thread>
#include <utility>
#include <cstring>
#include <map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace {

using namespace xerenge::plume_d3d;

rex::system::IGraphicsSystem* ActiveGraphicsSystem() {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state || !kernel_state->emulator()) {
    return nullptr;
  }
  return kernel_state->emulator()->graphics_system();
}

bool PlumePresentationActive() {
  auto* graphics = ActiveGraphicsSystem();
  return graphics && graphics->uses_direct_presentation();
}

template <typename T>
T* GuestPtr(uint32_t guest_address) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || guest_address == 0) {
    return nullptr;
  }
  return memory->TranslateVirtual<T*>(guest_address);
}

void CopyDispatchTable(uint32_t device_guest, uint32_t table_addr, uint32_t entries,
                       uint32_t dispatch_offset, uint32_t default_offset) {
  const auto* table = GuestPtr<const rex::be_u32>(table_addr);
  auto* dispatch = GuestPtr<rex::be_u32>(device_guest + dispatch_offset);
  auto* defaults = GuestPtr<rex::be_u32>(device_guest + default_offset);
  if (!table || !dispatch || !defaults) {
    return;
  }
  for (uint32_t i = 0; i < entries; ++i) {
    defaults[i] = table[i * 3 + 0];
    dispatch[i] = table[i * 3 + 1];
  }
}

void SeedDeviceRenderState(uint32_t device_guest) {
  if (auto* blend0 = GuestPtr<rex::be_u32>(device_guest + kDeviceRbBlendControl0Offset)) {
    *blend0 = kBlendControlNoBlend;
  }
  if (auto* blend1 = GuestPtr<rex::be_u32>(device_guest + kDeviceRbBlendControl1Offset)) {
    *blend1 = kBlendControlNoBlend;
    *(blend1 + 1) = kBlendControlNoBlend;
    *(blend1 + 2) = kBlendControlNoBlend;
  }
}

[[maybe_unused]] uint32_t PlumeCreateDevice(uint32_t out_device_guest) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return 1;
  }

  const uint32_t device_guest = memory->SystemHeapAlloc(kD3DDeviceAllocSize, 0x100);
  memory->Zero(device_guest, kD3DDeviceAllocSize);

  CopyDispatchTable(device_guest, kRenderStateTableGuestAddr, kRenderStateTableEntries,
                    kRenderStateDispatchOffset, kRenderStateDefaultOffset);
  CopyDispatchTable(device_guest, kSamplerStateTableGuestAddr, kSamplerStateTableEntries,
                    kSamplerStateDispatchOffset, kSamplerStateDefaultOffset);
  SeedDeviceRenderState(device_guest);

  if (auto* width = GuestPtr<rex::be_u32>(device_guest + kDeviceViewportOffset + 8)) {
    *width = 1280;
    *(width + 1) = 720;
  }
  if (auto* max_z = GuestPtr<rex::be_f32>(device_guest + kDeviceViewportOffset + 16)) {
    *max_z = 1.0f;
  }

  if (auto* out = GuestPtr<rex::be_u32>(out_device_guest)) {
    *out = device_guest;
  }

  REXLOG_INFO("plume: Direct3D_CreateDevice -> guest device {:08X}", device_guest);
  return 0;
}

[[maybe_unused]] void PlumeResetDevice(uint32_t device_guest, uint32_t params_guest) {
  auto* params = GuestPtr<rex::be_u32>(params_guest);
  if (!params) {
    return;
  }

  const uint32_t width = params[0];
  const uint32_t height = params[1];
  constexpr uint32_t kPresentParamsConfigDword = 17;
  const uint32_t cfg = params[kPresentParamsConfigDword];

  if (auto* res_width = GuestPtr<rex::be_u32>(device_guest + kDeviceResolutionWidthOffset)) {
    *res_width = width;
    *(res_width + 1) = height;
    *(res_width + 2) = width;
    *(res_width + 3) = cfg;
  }

  if (auto* viewport_width = GuestPtr<rex::be_u32>(device_guest + kDeviceViewportOffset + 8)) {
    *viewport_width = width;
    *(viewport_width + 1) = height;
  }

  if (auto* present = GuestPtr<uint8_t>(device_guest + kDevicePresentParamsOffset)) {
    std::memcpy(present, params, 124);
  }
  if (auto* applied = GuestPtr<uint8_t>(device_guest + kDeviceResolutionAppliedOffset)) {
    *applied |= 0x10;
  }

  REXLOG_INFO("plume: D3DDevice_Reset {}x{}", width, height);
}


// How long the title spends inside each of the device's wait routines. Counting
// entries said almost nothing: a wait entered thirty times can still own the
// whole frame if each entry lasts fifty milliseconds, and that is exactly the
// shape a stalled frame loop takes.
struct BlockTimer {
  std::chrono::steady_clock::time_point started;
  size_t index;
  explicit BlockTimer(size_t which)
      : started(xerenge::Diagnostics() ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{}),
        index(which) {}
  ~BlockTimer();
};

namespace {
std::atomic<uint64_t> g_block_ns[5];
std::atomic<uint64_t> g_block_calls[5];
const char* const g_block_names[5] = {"SecondaryPosition", "PrimaryRange", "Fence", "RingSpace",
                                      "KickOff"};
}  // namespace

BlockTimer::~BlockTimer() {
  if (!xerenge::Diagnostics()) {
    return;
  }
  const uint64_t took = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  g_block_ns[index].fetch_add(took, std::memory_order_relaxed);
  g_block_calls[index].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint64_t> last_ms{0};
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  uint64_t was = last_ms.load(std::memory_order_relaxed);
  if (now - was >= 2000 && last_ms.compare_exchange_strong(was, now)) {
    std::string report;
    for (size_t i = 0; i < 5; ++i) {
      const uint64_t ns = g_block_ns[i].load(std::memory_order_relaxed);
      const uint64_t calls = g_block_calls[i].load(std::memory_order_relaxed);
      report += fmt::format(" {}={}ms/{}", g_block_names[i], ns / 1000000, calls);
    }
    REXLOG_INFO("plume: time inside the device's waits:{}", report);
  }
}

void PlumePresentFromDevice(uint32_t device_guest) {
  auto* graphics = ActiveGraphicsSystem();
  if (!graphics) {
    return;
  }

  uint32_t width = 1280;
  uint32_t height = 720;
  if (auto* viewport_width = GuestPtr<rex::be_u32>(device_guest + kDeviceViewportOffset + 8)) {
    width = *viewport_width;
    height = *(viewport_width + 1);
  }
  if (width == 0 || height == 0) {
    if (auto* res_width = GuestPtr<rex::be_u32>(device_guest + kDeviceResolutionWidthOffset)) {
      width = *res_width;
      height = *(res_width + 1);
    }
  }

  graphics->PresentGuestFrame(width, height);
}

struct BeginVerticesScratch {
  uint32_t va = 0;
  uint32_t capacity = 0;
} g_begin_vertices_scratch;

struct BeginVerticesPending {
  uint32_t device = 0;
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t data_va = 0;
} g_begin_vertices_pending;

uint32_t D3DDevice_BeginVertices_hook(uint32_t device_guest, uint32_t primitive_type,
                                      uint32_t vertex_count, uint32_t vertex_stride) {
  g_begin_vertices_pending = {};
  const uint64_t size = uint64_t(vertex_count) * uint64_t(vertex_stride);
  if (size == 0 || size > 0xFFFFFFFFull) {
    return 0;
  }

  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return 0;
  }
  if (g_begin_vertices_scratch.capacity < size) {
    const uint32_t new_capacity = uint32_t(size);
    const uint32_t va = memory->SystemHeapAlloc(new_capacity, 0x20);
    if (!va) {
      return 0;
    }
    if (g_begin_vertices_scratch.va) {
      memory->SystemHeapFree(g_begin_vertices_scratch.va);
    }
    g_begin_vertices_scratch.va = va;
    g_begin_vertices_scratch.capacity = new_capacity;
  }

  g_begin_vertices_pending.device = device_guest;
  g_begin_vertices_pending.primitive_type = primitive_type;
  g_begin_vertices_pending.vertex_count = vertex_count;
  g_begin_vertices_pending.stride = vertex_stride;
  g_begin_vertices_pending.data_va = g_begin_vertices_scratch.va;

  if (auto* graphics = ActiveGraphicsSystem()) {
    graphics->BindGuestD3DDevice(device_guest);
  }

  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1) < 8) {
    REXLOG_INFO("plume: BeginVertices prim={} count={} stride={} -> {:08X}", primitive_type,
                vertex_count, vertex_stride, g_begin_vertices_scratch.va);
  }
  return g_begin_vertices_scratch.va;
}

uint32_t D3DDevice_EndVertices_hook(uint32_t /*device_guest*/) {
  const BeginVerticesPending pending = g_begin_vertices_pending;
  g_begin_vertices_pending = {};
  if (!pending.data_va || !pending.vertex_count) {
    return 0;
  }
  if (auto* graphics = ActiveGraphicsSystem()) {
    graphics->SubmitGuestDrawVerticesUP(pending.primitive_type, pending.vertex_count,
                                        pending.stride, pending.data_va);
  }
  return 0;
}

void BeginVerticesRuntime(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<D3DDevice_BeginVertices_hook>(ctx, base);
}

void EndVerticesRuntime(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<D3DDevice_EndVertices_hook>(ctx, base);
}

bool PatchGuestFunction(rex::runtime::FunctionDispatcher* dispatcher, uint32_t guest_address,
                        PPCFunc* func, const char* name) {
  if (dispatcher->SetFunction(guest_address, func)) {
    REXLOG_INFO("plume: runtime hook {} @ {:08X}", name, guest_address);
    return true;
  }
  REXLOG_ERROR("plume: failed to runtime-hook {} @ {:08X}", name, guest_address);
  return false;
}

}  // namespace

void xerenge::plume_d3d::InstallPlumeRuntimeHooks() {
  if (!PlumePresentationActive()) {
    return;
  }

  static std::atomic<bool> installed{false};
  if (installed.exchange(true)) {
    return;
  }

  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state || !kernel_state->function_dispatcher()) {
    REXLOG_ERROR("plume: cannot install runtime D3D hooks (no dispatcher)");
    installed = false;
    return;
  }
  auto* dispatcher = kernel_state->function_dispatcher();

  // Burnout issues draws through the Xenos ring, not only BeginVertices.
  // Leave ring helpers on the original D3D code; plume consumes PM4 on WPTR.
  //
  // These two replacements are the only guest code this backend substitutes,
  // and the xenos backend substitutes none - so they are the one thing that
  // can make the title behave differently here for reasons of our own. Being
  // able to leave the title's own code in place says whether they are why the
  // render thread stops calling its kick builder.
  if (std::getenv("XERENGE_UP_PATCH") == nullptr) {
    REXLOG_INFO("plume: leaving D3DDevice_Begin/EndVertices on the title's own code");
    return;
  }
  PatchGuestFunction(dispatcher, kBeginVerticesGuestAddr, &BeginVerticesRuntime,
                     "D3DDevice_BeginVertices");
  PatchGuestFunction(dispatcher, kEndVerticesGuestAddr, &EndVerticesRuntime,
                     "D3DDevice_EndVertices");
}

extern "C" void __imp__D3DDevice_SetVertexShader(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetPixelShader(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetVertexDeclaration(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetStreamSource(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetIndices(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetTexture(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_SetRenderTarget(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_DrawVertices(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_BeginVertices(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_BeginTiling(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_EndTiling(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_CreateTexture(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_CreateVertexBuffer(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_CreateIndexBuffer(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__Direct3D_CreateDevice(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8238CD28(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82382710(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82381920(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82382200(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82382578(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_Swap(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_DrawVerticesUP(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_DrawIndexedVertices(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_ClearF(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8238AC88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82387920(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8238B0F0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3D_BlockOnPrimaryRange(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3D_BlockOnFence(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3D_KickOff(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8246E100(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8246E158(PPCContext& __restrict, uint8_t*);


namespace {
// One frame of Direct3D calls, in order, as the title makes them:
// XERENGE_TRACE_FRAME=<n> records between the n-th and the next Swap and logs
// it with repeats folded. Answers "what draws after X" without guessing.
std::mutex g_trace_mutex;
std::vector<std::string> g_trace;
std::atomic<bool> g_tracing{false};

void Trace(std::string entry) {
  if (!g_tracing.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(g_trace_mutex);
  if (g_trace.size() < 4000) {
    g_trace.push_back(std::move(entry));
  }
}

void TraceSwap() {
  static const uint64_t wanted = [] {
    const char* v = std::getenv("XERENGE_TRACE_FRAME");
    return v ? std::strtoull(v, nullptr, 10) : 0ull;
  }();
  static std::atomic<uint64_t> swaps{0};
  const uint64_t n = swaps.fetch_add(1) + 1;
  // A capture was just written: trace the next frame to go with it.
  static bool traced_for_capture = false;
  if (!g_tracing.load() && ActiveGraphicsSystem() &&
      ActiveGraphicsSystem()->TakeCallTraceRequest()) {
    traced_for_capture = true;
    g_tracing.store(true);
    return;
  }
  const bool ends_capture_trace = traced_for_capture;
  if (!wanted && !ends_capture_trace) {
    return;
  }
  if (wanted && n == wanted) {
    g_tracing.store(true);
    return;
  }
  if ((ends_capture_trace || n == wanted + 1) && g_tracing.exchange(false)) {
    traced_for_capture = false;
    std::lock_guard lock(g_trace_mutex);
    std::string out;
    size_t i = 0;
    while (i < g_trace.size()) {
      size_t j = i;
      while (j < g_trace.size() && g_trace[j] == g_trace[i]) {
        ++j;
      }
      out += j - i > 1 ? fmt::format("\n  {} x{}", g_trace[i], j - i)
                       : fmt::format("\n  {}", g_trace[i]);
      i = j;
    }
    REXLOG_INFO("plume: Direct3D calls of frame {}:{}", wanted, out);
  }
}

}  // namespace

REX_HOOK_RAW(D3D_BlockOnPrimaryRange) {
  BlockTimer block_timer{1};
  __imp__D3D_BlockOnPrimaryRange(ctx, base);
}

REX_HOOK_RAW(D3D_BlockOnFence) {
  BlockTimer block_timer{2};
  __imp__D3D_BlockOnFence(ctx, base);
}

REX_HOOK_RAW(D3D_KickOff) {
  BlockTimer block_timer{4};
  __imp__D3D_KickOff(ctx, base);
}

REX_HOOK_RAW(sub_8246E100) {
  __imp__sub_8246E100(ctx, base);
}

REX_HOOK_RAW(sub_8246E158) {
  if (PlumePresentationActive()) {
    xerenge::plume_d3d::InstallPlumeRuntimeHooks();
  }
  __imp__sub_8246E158(ctx, base);
}

REX_HOOK_RAW(Direct3D_CreateDevice) {
  if (PlumePresentationActive()) {
    xerenge::plume_d3d::InstallPlumeRuntimeHooks();
  }
  // 0x8246B710 is a mid-function fragment, not CreateDevice's prologue.
  __imp__Direct3D_CreateDevice(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Swap) {
  TraceSwap();
  if (auto* graphics = ActiveGraphicsSystem()) {
    graphics->NoteGuestFrameEnd();
  }
  // This used to present from here with a device address of zero, which reads
  // its width and height out of guest memory near the null page - garbage - and
  // then does a full present on a guest thread. The address it is attached to
  // is a post-bctrl epilogue rather than a function entry, so it fires at a
  // point of its own choosing, and VdSwap already presents properly. That is
  // work the title pays for, off its own hot path, for a frame nobody wants.
  // Present from here when the command ring has stopped doing it. Entering a
  // scene, this title stops submitting to the primary ring entirely - the walk
  // executes no more buffers and no more swap packets arrive - while it goes
  // on issuing thousands of draws through these calls. Presentation was driven
  // only by that ring, so the screen froze on the last frame that got through,
  // and everything drawn afterwards was never shown.
  //
  // The ring keeps ownership while it is working: this only fires once nothing
  // has reached the screen for three frames, so the menu is untouched.
  static const bool present_always = std::getenv("XERENGE_SWAP_HOOK_PRESENT") != nullptr;
  if (PlumePresentationActive()) {
    auto* graphics = ActiveGraphicsSystem();
    if (present_always || (graphics && !graphics->PresentedRecently())) {
      static std::atomic<uint64_t> from_swap{0};
      const uint64_t n = from_swap.fetch_add(1) + 1;
      if (n <= 4 || (n % 200) == 0) {
        REXLOG_INFO("plume: presenting from the title's Swap, the ring has gone quiet (#{})", n);
      }
      PlumePresentFromDevice(ctx.r3.u32);
    }
    static std::atomic<uint64_t> swaps{0};
    const uint64_t s = swaps.fetch_add(1) + 1;
    if (s <= 2 || (s % 500) == 0) {
      REXLOG_INFO("plume: D3D Swap #{} (ring presented recently: {})", s,
                  graphics && graphics->PresentedRecently());
    }
  }
  __imp__D3DDevice_Swap(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawVerticesUP) {
  Trace("DrawVerticesUP");
  // 0x8248FBC8 is a stack-restore epilogue, not DrawVerticesUP.
  __imp__D3DDevice_DrawVerticesUP(ctx, base);
}

// The routine the title spins in when it believes the ring is full. It reaches
// its bookkeeping through the object in r3, so hooking it hands us the device
// address outright - no searching the heap, which is neither safe nor cheap.
//
// This sits on a hot path, so the common case must cost nothing beyond one
// relaxed load: everything below runs on the first call only.
REX_HOOK_RAW(sub_8238CD28) {
  BlockTimer block_timer{3};
  // It waits for the value at the published address to reach the one held in
  // the object at +8, so both are needed to say what it is short of. While
  // stalled it spins here, so sampling rarely still catches the state - and
  // costs one atomic increment per call the rest of the time.
  static std::atomic<uint64_t> calls{0};
  if ((calls.fetch_add(1, std::memory_order_relaxed) & 0xFFFFFu) == 0) {
    const uint32_t object = ctx.r3.u32;
    const auto* slot = GuestPtr<const rex::be_u32>(object);
    const uint32_t device = slot ? static_cast<uint32_t>(*slot) : 0u;
    uint32_t fields[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
      const auto* at = GuestPtr<const rex::be_u32>(object + i * 4);
      fields[i] = at ? static_cast<uint32_t>(*at) : 0u;
    }
    // The caller reached this with r3 = its own stack frame + 80, and it keeps
    // the value it is waiting for at frame + 156 - so that is object + 76.
    const auto* target_at = GuestPtr<const rex::be_u32>(object + 76);
    const uint32_t awaited = target_at ? static_cast<uint32_t>(*target_at) : 0u;
    (void)fields;
    uint32_t published_at = 0;
    uint32_t published = 0;
    if (device) {
      const auto* p = GuestPtr<const rex::be_u32>(device + 10384);
      published_at = p ? static_cast<uint32_t>(*p) : 0u;
      if (published_at) {
        const auto* v = GuestPtr<const rex::be_u32>(published_at);
        published = v ? static_cast<uint32_t>(*v) : 0u;
      }
    }
    uint32_t limit = 0;
    if (device) {
      const auto* l = GuestPtr<const rex::be_u32>(device + 10396);
      limit = l ? static_cast<uint32_t>(*l) : 0u;
    }
    REXLOG_INFO(
        "plume: ring wait: object [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]; "
        "{:08X} publishes {:08X}, limit {:08X}, called from {:08X}",
        fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], published_at,
        published, limit, static_cast<uint32_t>(ctx.lr));
    (void)awaited;
    if (auto* graphics = ActiveGraphicsSystem()) {
      graphics->BindGuestD3DDevice(device);
    }
  }
  __imp__sub_8238CD28(ctx, base);
}

// Computes the progress value the ring wait then spins on, leaving it at the
// address passed in r4. Reading it back after the call is the only way to see
// what the title is actually waiting for - the wait loop keeps it in a
// register, and the stack slot it came from belongs to a frame we cannot name.
REX_HOOK_RAW(sub_82382710) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82382710(ctx, base);
  static std::atomic<uint64_t> calls{0};
  if ((calls.fetch_add(1, std::memory_order_relaxed) & 0x3FFFu) == 0) {
    const auto* at = GuestPtr<const rex::be_u32>(out);
    REXLOG_INFO("plume: ring wait wants {:08X}", at ? static_cast<uint32_t>(*at) : 0u);
  }
}

// CDevice::BlockOnSecondaryPosition in the beta symbols. It waits on the
// second word of the GPU identifier block - the secondary command buffer's
// position - while everything published so far has gone to the first word.
// Logged with a sweep of the device's own fields, so the value it waits for
// can be matched to whichever field the title keeps it in.
REX_HOOK_RAW(sub_82381920) {
  BlockTimer block_timer{0};
  // It spins inside, so entries are few and the last one is the one that never
  // returns - sampling every millionth call only ever showed the first.
  // Nearly every entry passes its first test and returns at once, so counting
  // entries only ever captured ones that did not block. Evaluate the routine's
  // own exit test here instead and report only an entry that will spin.
  const uint32_t device_now = ctx.r3.u32;
  uint32_t secondary_now = 0;
  if (const auto* b = GuestPtr<const rex::be_u32>(device_now + 10384)) {
    if (const auto* q = GuestPtr<const rex::be_u32>(static_cast<uint32_t>(*b) + 4)) {
      secondary_now = static_cast<uint32_t>(*q);
    }
  }
  // Bind the device here, not inside the reporting branch below: that branch
  // only runs when this routine is about to block, so fixing the blocking took
  // the address away with it and the renderer went back to not knowing where
  // to publish. Once is enough, and the common path costs one relaxed load.
  static std::atomic<bool> bound{false};
  if (!bound.load(std::memory_order_relaxed)) {
    if (const auto* b = GuestPtr<const rex::be_u32>(device_now + 10384)) {
      if (static_cast<uint32_t>(*b) != 0) {
        bound.store(true, std::memory_order_relaxed);
        if (auto* graphics = ActiveGraphicsSystem()) {
          graphics->BindGuestD3DDevice(device_now);
        }
        REXLOG_INFO("plume: bound D3D device {:08X} from the secondary-position wait", device_now);
      }
    }
  }

  // The primary ring wait (sub_823819D0) spins while the read pointer it sees
  // at devblock+60 lies in the span it is about to overwrite, and it compares
  // that against its own write pointer at dev+10444, both as dword indices
  // masked with dev+14000. Report all three together: the backend writes that
  // slot but has never checked what the title reads back from it.
  const uint32_t phase = (ctx.r5.u32 - secondary_now) & 3u;
  const bool will_block =
      phase != 0 && (phase != 1 || ctx.r4.u32 > (secondary_now & ~3u));
  static std::atomic<uint64_t> calls{0};
  if (will_block && calls.fetch_add(1, std::memory_order_relaxed) < 256) {
    const uint32_t device = ctx.r3.u32;
    const uint32_t r4 = ctx.r4.u32;
    const uint32_t target = ctx.r5.u32;
    uint32_t block = 0, primary = 0, secondary = 0;
    if (const auto* b = GuestPtr<const rex::be_u32>(device + 10384)) {
      block = static_cast<uint32_t>(*b);
      if (const auto* p = GuestPtr<const rex::be_u32>(block)) primary = static_cast<uint32_t>(*p);
      if (const auto* q = GuestPtr<const rex::be_u32>(block + 4)) secondary = static_cast<uint32_t>(*q);
    }
    // Exact equality found nothing, so look across the whole device for fields
    // near the position being waited on - that is where the title must keep
    // its own secondary write pointer - and for any copy of what the GPU last
    // reported.
    std::string matches;
    for (uint32_t off = 0; off <= 16384; off += 4) {
      const auto* f = GuestPtr<const rex::be_u32>(device + off);
      if (!f) {
        continue;
      }
      const uint32_t v = static_cast<uint32_t>(*f);
      if (v >= r4 - 0x400u && v <= r4 + 0x400u) {
        matches += fmt::format(" +{}~pos {:08X}", off, v);
      } else if (v == secondary) {
        matches += fmt::format(" +{}=gpu {:08X}", off, v);
      }
    }
    // Sampled on a call counter this only ever caught the opening moments, when
    // the device is not even bound yet - report it where it matters instead.
    {
      const auto rd = [&](uint32_t at) -> uint32_t {
        const auto* q = GuestPtr<const rex::be_u32>(at);
        return q ? static_cast<uint32_t>(*q) : 0u;
      };
      // The display-list ring the lap gate accounts for: the colleague's
      // teardown puts its base at dev+14004 and the primary ring's own base
      // and mask at dev+13996/+14000. Read all of them beside the position
      // being waited on, so a lap can be counted against real bounds instead
      // of guessed from an address going backwards.
      REXLOG_INFO(
          "plume: ring accounting: rptr slot holds {:08X}, title write pointer {:08X}; "
          "list ring base {:08X} cursor {:08X} lap {:08X}; primary base {:08X} mask {:08X}",
          block ? rd(block + 60) : 0u, rd(device + 10444), rd(device + 14004),
          rd(device + 14016), rd(device + 14020), rd(device + 13996), rd(device + 14000));
    }
    REXLOG_INFO(
        "plume: BlockOnSecondaryPosition r4={:08X} target={:08X}; block {:08X} holds "
        "primary {:08X} secondary {:08X}; device fields:{}",
        r4, target, block, primary, secondary, matches.empty() ? " none" : matches);
  }
  __imp__sub_82381920(ctx, base);
}

// The kick builder: where the title decides to hand a segment to the GPU and
// rings the doorbell. When the picture stops and the ring goes quiet, this
// says which side stopped - if kicks keep coming the fault is ours, and if
// they stop the title decided there was nothing to submit.
REX_HOOK_RAW(sub_82382200) {
  static std::atomic<uint64_t> kicks{0};
  static std::atomic<uint64_t> last_report_ms{0};
  const uint64_t n = kicks.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  uint64_t last = last_report_ms.load(std::memory_order_relaxed);
  if (now_ms - last >= 1000 && last_report_ms.compare_exchange_strong(last, now_ms)) {
    REXLOG_INFO("plume: {} kick(s) so far", n);
  }
  __imp__sub_82382200(ctx, base);
}

// Observation only, for now: count the calls and forward. The point is to
// prove the recovered addresses are the real entry points before any state is
// built on them - the dispatcher-based attempt saw nothing because the
// recompiled code calls these directly, so only a link-time hook reaches them.
namespace {
std::atomic<uint64_t> g_d3d_counts[16] = {};
const char* const g_d3d_names[] = {
    "SetVertexShader", "SetPixelShader", "SetVertexDeclaration", "SetStreamSource",
    "SetIndices",      "SetTexture",     "SetRenderTarget",      "DrawVertices",
    "BeginVertices",   "BeginTiling",    "EndTiling",            "CreateTexture",
    "CreateVertexBuffer", "CreateIndexBuffer"};

void NoteD3D(size_t which, PPCContext& ctx) {
  if (!xerenge::Diagnostics()) {
    return;
  }
  const uint64_t n = g_d3d_counts[which].fetch_add(1, std::memory_order_relaxed);
  if (n == 0) {
    REXLOG_INFO("plume: D3D {} first call r3={:08X} r4={:08X} r5={:08X}", g_d3d_names[which],
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  static std::atomic<uint64_t> last_ms{0};
  // The clock only now and then: read on every call it was a twentieth of the
  // title's thread on the Pixel.
  if ((n & 1023u) != 0) {
    return;
  }
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t was = last_ms.load(std::memory_order_relaxed);
  if (now - was >= 5000 && last_ms.compare_exchange_strong(was, now)) {
    std::string line;
    for (size_t i = 0; i < 14; ++i) {
      line += fmt::format(" {}={}", g_d3d_names[i],
                          g_d3d_counts[i].load(std::memory_order_relaxed));
    }
    REXLOG_INFO("plume: D3D calls:{}", line);
  }
}
}  // namespace

// The render state as the title sets it, gathered from its own D3D calls.
//
// This is the shape the reference implementation uses and the reason it never
// has to parse the command ring: everything a draw needs is an argument to one
// of these calls, so there is nothing to reconstruct from a register file and
// nothing to wait for. For now it only describes the draw it could issue -
// proving the inputs are all here and correct - before anything is handed to
// the renderer.
namespace {

struct D3DStream {
  uint32_t buffer = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
};

struct D3DRenderState {
  uint32_t device = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t vertex_declaration = 0;
  uint32_t index_buffer = 0;
  uint32_t render_target = 0;
  D3DStream streams[4];
  uint32_t textures[16] = {};
};

std::mutex g_state_mutex;
D3DRenderState g_state;


// Which entry point the title actually draws through, and how much geometry
// goes each way. The counters NoteD3D keeps do not include the indexed draw,
// and that is the one the scene is most likely to use.
void CountDrawPath(const char* what, uint32_t count) {
  static std::mutex mutex;
  // Keyed by the literal itself: a std::string made per draw, under a lock,
  // cost more than the draw's bookkeeping is worth.
  static std::map<const char*, std::pair<uint64_t, uint64_t>> totals;
  static std::atomic<uint64_t> last_ms{0};
  static std::atomic<uint32_t> calls{0};
  {
    std::lock_guard lock(mutex);
    auto& entry = totals[what];
    entry.first += 1;
    entry.second += count;
  }
  if ((calls.fetch_add(1, std::memory_order_relaxed) & 255u) != 0) {
    return;
  }
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  uint64_t was = last_ms.load(std::memory_order_relaxed);
  if (now - was < 5000 || !last_ms.compare_exchange_strong(was, now)) {
    return;
  }
  std::string line;
  {
    std::lock_guard lock(mutex);
    for (const auto& [name, counts] : totals) {
      line += fmt::format(" {}={}calls/{}idx", name, counts.first, counts.second);
    }
  }
  REXLOG_INFO("plume: draw paths:{}", line);
}


// A buffer argument can be either the data itself or the small object that
// owns it - the object keeps its data pointer at +0x0C. Passing the object
// address on as though it were data makes the first words of the header get
// read as vertices or indices, which is exactly the shape of the geometry the
// direct path was producing.
uint32_t BufferDataAddress(uint32_t handle) {
  if (handle < 0x10000000u) {
    return handle;
  }
  // Which of these is an object and which is already the data cannot be told
  // from the address. The interface's buffers are allocated in the title's own
  // heap and the scene's are allocated in the window the GPU reads through, so
  // ruling out that window skipped the step for every scene buffer and handed
  // back the address of the object itself - which is why scene draws were
  // reading a resource header where vertices should have been.
  //
  // So ask the memory instead. What follows the handle either is a fetch
  // constant - an address in the window the GPU reads through, carrying the
  // format in its low bits - or it is not, and then the handle was the data
  // all along.
  const auto* data = GuestPtr<const rex::be_u32>(handle + 0x0C);
  const uint32_t address = data ? (static_cast<uint32_t>(*data) & ~uint32_t(0x3)) : 0u;
  // Both windows the GPU reads through: the race's world keeps its buffers in
  // 0xF0000000 and up, and treating those objects as data fed their headers in
  // as vertices.
  if ((address & 0xE0000000u) != 0xE0000000u) {
    return handle;
  }
  // The low bits of a buffer pointer are not part of the address - they carry
  // the format and byte order the GPU should read it with. Left in place they
  // shift every read by a byte or three, which assembles vertices out of the
  // middle of other vertices.
  return address;
}

// Width of a render target surface, from its size word at +0x18: width - 1 in
// bits 0-12, height - 1 in bits 13-25. Zero if there is no surface.
uint32_t SurfaceWidth(uint32_t surface) {
  if (surface < 0x10000000u) {
    return 0;
  }
  const auto* w = GuestPtr<const rex::be_u32>(surface + 0x18);
  return w ? (static_cast<uint32_t>(*w) & 0x1FFFu) + 1 : 0u;
}

// A render target surface's own size: word +0x18, width - 1 in bits 0-12 and
// height - 1 in bits 13-25.
void SurfaceSize(uint32_t surface, uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
  if (surface < 0x10000000u) {
    return;
  }
  if (const auto* w = GuestPtr<const rex::be_u32>(surface + 0x18)) {
    const uint32_t word = *w;
    width = (word & 0x1FFFu) + 1;
    height = ((word >> 13) & 0x1FFFu) + 1;
  }
}

// Whether what is being drawn now is meant for the frame itself. The scene is
// drawn into several targets: the frame, in 1280-wide tiles, and smaller ones
// for the car's reflection (128x128) and for glow (320x180). Only the first
// belongs on the screen. Drawing the others there as well laid a second,
// differently-aimed view of the garage over the first, and the glow passes
// over both - the blue planes, and the screen split into regions.
//
// The tiles need no care: the title issues each draw once, in the coordinates
// of the whole frame, and splitting it into tiles happens below this point.
bool RenderTargetsFromDirect3D() {
  static const bool on = std::getenv("XERENGE_D3D_TARGETS") != nullptr;
  return on;
}

bool DrawingIntoFrame(uint32_t surface) {
  // With render targets followed, every target is drawn - into one buffer, in
  // order, the way the console draws them all into its one EDRAM - and the
  // title's own clears and resolves keep them apart.
  if (RenderTargetsFromDirect3D()) {
    return true;
  }
  return SurfaceWidth(surface) >= 1280;
}

// A guest address as Direct3D hands it to the GPU: the window bits dropped,
// and the page the 0xE0000000-and-up windows sit ahead of added back - the
// computation the beta's own code does inline wherever it writes an address
// into a packet.
uint32_t GpuPhysical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + ((((address >> 20) + 0x200u) & 0x1000u));
}

// The state every Direct3D draw carries with it, whatever kind of draw it is:
// the bound shader objects and textures.
void TakeDrawState(rex::system::IGraphicsSystem::GuestDrawBuffers& buffers) {
  std::lock_guard lock(g_state_mutex);
  buffers.vertex_shader_object = g_state.vertex_shader;
  buffers.pixel_shader_object = g_state.pixel_shader;
  for (uint32_t slot = 0; slot < 8; ++slot) {
    buffers.textures[slot] = g_state.textures[slot];
  }
  // Surface word +0x18: width - 1 in bits 0-12, height - 1 in bits 13-25.
  if (g_state.render_target >= 0x10000000u) {
    if (const auto* size = GuestPtr<const rex::be_u32>(g_state.render_target + 0x18)) {
      const uint32_t word = *size;
      buffers.target_width = (word & 0x1FFFu) + 1;
      buffers.target_height = ((word >> 13) & 0x1FFFu) + 1;
    }
  }
}

uint32_t CurrentRenderTarget() {
  std::lock_guard lock(g_state_mutex);
  return g_state.render_target;
}

// The interface drawn from the Direct3D calls rather than the command ring.
// Opt-in while it is new: the ring still draws the interface in the menus.
bool InterfaceFromDirect3D() {
  static const bool on = std::getenv("XERENGE_D3D_UI") != nullptr;
  return on;
}

void CountFrameRouting(bool into_frame) {
  if (!xerenge::Diagnostics()) {
    return;
  }
  static std::atomic<uint64_t> kept{0};
  static std::atomic<uint64_t> elsewhere{0};
  static std::atomic<uint64_t> last_ms{0};
  const uint64_t counted =
      (into_frame ? kept : elsewhere).fetch_add(1, std::memory_order_relaxed);
  if ((counted & 255u) != 0) {
    return;
  }
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  uint64_t was = last_ms.load(std::memory_order_relaxed);
  if (now - was >= 3000 && last_ms.compare_exchange_strong(was, now)) {
    REXLOG_INFO("plume: Direct3D draws into the frame {}, into other targets {} (not drawn yet)",
                kept.load(std::memory_order_relaxed), elsewhere.load(std::memory_order_relaxed));
  }
}

void DescribeDraw(const char* what, uint32_t prim_type, uint32_t base_vertex, uint32_t start_index,
                  uint32_t primitive_count) {
  if (!xerenge::Diagnostics()) {
    return;
  }
  CountDrawPath(what, primitive_count);
  // Count the big draws apart from the small ones. One shared allowance is
  // spent entirely on the interface's three-index draws before a scene is ever
  // reached, so the draws this path exists for never got described at all.
  static std::atomic<uint64_t> described_small{0};
  static std::atomic<uint64_t> described_big{0};
  auto& counter = primitive_count >= 64 ? described_big : described_small;
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  if (n >= 8 && (n % 20000) != 0) {
    return;
  }
  std::lock_guard lock(g_state_mutex);
  REXLOG_INFO(
      "plume: D3D {} prim={} base={} start={} prims={} | vs={:08X} ps={:08X} decl={:08X} "
      "ib={:08X} vb0={:08X}+{:X} stride={} tex0={:08X} rt={:08X}",
      what, prim_type, base_vertex, start_index, primitive_count, g_state.vertex_shader,
      g_state.pixel_shader, g_state.vertex_declaration, g_state.index_buffer,
      g_state.streams[0].buffer, g_state.streams[0].offset, g_state.streams[0].stride,
      g_state.textures[0], g_state.render_target);
}

}  // namespace

// Where Direct3D hands the GPU its shaders. Both routines are the beta's, at
// the same 0xAB0 displacement as every other Direct3D function in this build.
//
// D3D::IncrementalShaderPatchAndLoad(device, flag, vertex shader, ...) copies
// the vertex shader's microcode into the command buffer and then patches the
// copy to match the vertex declaration. The object's own microcode stays
// unpatched, so the copy is the only place the bytes the GPU runs exist:
// size at vertex shader + 0x258, and the copy ends where the device's write
// pointer (device + 0) is left.
//
// D3D::LazyWriteShaders(device) loads the current shaders by pointer instead:
// the vertex shader from +0x28 (patched in place first) with its size at
// +0x258, the pixel shader from +0x0C with its size at +0x3C. The current
// objects sit at device + 0x3294 and + 0x3290.
thread_local bool g_vertex_shader_loaded_incrementally = false;

uint32_t GuestWord(uint32_t address) {
  const auto* w = GuestPtr<const rex::be_u32>(address);
  return w ? static_cast<uint32_t>(*w) : 0u;
}

REX_HOOK_RAW(sub_8238AC88) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t vertex_shader = ctx.r5.u32;
  __imp__sub_8238AC88(ctx, base);
  auto* graphics = ActiveGraphicsSystem();
  if (!graphics || !device || !vertex_shader) {
    return;
  }
  const uint32_t size = GuestWord(vertex_shader + 0x258);
  const uint32_t end = GuestWord(device);
  if (size != 0 && end > size) {
    graphics->NoteShaderLoad(vertex_shader, false, end - size + 4, size);
    g_vertex_shader_loaded_incrementally = true;
  }
}

REX_HOOK_RAW(sub_8238B0F0) {
  const uint32_t device = ctx.r3.u32;
  g_vertex_shader_loaded_incrementally = false;
  __imp__sub_8238B0F0(ctx, base);
  auto* graphics = ActiveGraphicsSystem();
  if (!graphics || !device) {
    return;
  }
  if (const uint32_t pixel_shader = GuestWord(device + 0x3290)) {
    graphics->NoteShaderLoad(pixel_shader, true, GuestWord(pixel_shader + 0x0C),
                             GuestWord(pixel_shader + 0x3C));
  }
  const uint32_t vertex_shader = GuestWord(device + 0x3294);
  if (vertex_shader && !g_vertex_shader_loaded_incrementally) {
    graphics->NoteShaderLoad(vertex_shader, false, GuestWord(vertex_shader + 0x28),
                             GuestWord(vertex_shader + 0x258));
  }
}

// D3DDevice_Resolve(device, flags, source rect, destination texture, ...) -
// the beta's, at 0x82386E70 + 0xAB0. The render target as it stands is copied
// into the destination texture; the marker keeps that copy's place among the
// draws, and the texture's fetch constant (+0x10, base in the second word)
// says where the GPU will later read it from.
REX_HOOK_RAW(sub_82387920) {
  Trace(fmt::format("Resolve flags={:X} dest={:08X}", ctx.r4.u32, ctx.r6.u32 >= 0x10000000u ? GpuPhysical(GuestWord(ctx.r6.u32 + 0x14) & 0xFFFFF000u) : 0u));
  if (RenderTargetsFromDirect3D()) {
    if (auto* graphics = ActiveGraphicsSystem()) {
      const uint32_t flags = ctx.r4.u32;
      const uint32_t texture = ctx.r6.u32;
      if (texture >= 0x10000000u) {
        const uint32_t base_word = GuestWord(texture + 0x14);
        // The fetch constant's dword 5, bits 9-10: 3 is a cube map, and then
        // r9 names the face the copy lands in.
        const bool cube = ((GuestWord(texture + 0x24) >> 9) & 0x3u) == 3u;
        uint32_t width = 0;
        uint32_t height = 0;
        SurfaceSize(CurrentRenderTarget(), width, height);
        graphics->NoteGuestResolve(flags, GpuPhysical(base_word & 0xFFFFF000u), width, height,
                                   cube ? ctx.r9.u32 : 0u, cube);
      }
    }
  }
  __imp__sub_82387920(ctx, base);
}

// D3DDevice_ClearF(device, flags, count, rects, color, float z, stencil). The z
// arrives in f1, the flags in r4: 0x10 is depth on this console.
REX_HOOK_RAW(D3DDevice_ClearF) {
  Trace(fmt::format("Clear flags={:X}", ctx.r4.u32));
  // Clearing the reflection or glow target must not reach the frame's depth
  // buffer - there is only one here, and it would be wiped mid-scene.
  uint32_t target = 0;
  {
    std::lock_guard lock(g_state_mutex);
    target = g_state.render_target;
  }
  const bool is_main_target = (SurfaceWidth(target) >= 1280);
  auto* graphics = DrawingIntoFrame(target) ? ActiveGraphicsSystem() : nullptr;
  if (graphics && RenderTargetsFromDirect3D()) {
    // ClearF(device, flags, count, rects, const D3DVECTOR4* color, z, stencil)
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (const uint32_t at = ctx.r7.u32) {
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bits = GuestWord(at + i * 4);
        std::memcpy(&color[i], &bits, 4);
      }
    }
    const bool whole_target = (ctx.r5.u32 == 0) && is_main_target;
    // A smaller target - a face of the cars' reflection cube, a glow buffer -
    // is drawn in the corner of the one buffer at its own size, so its clear
    // is that corner, colour and depth. Dropped, as it used to be, the faces
    // were drawn against the frame's uncleared depth and all failed the test,
    // and the reflection the cars sample was the frame's clear colour alone,
    // which their shader scales up into bright blue patches. The size rides
    // in the flags' unused high bits (see NoteGuestClearColor): bit 30 set,
    // width - 1 in bits 8-18, height - 1 in bits 19-29. Rectangle clears of
    // such a target stay dropped.
    uint32_t flags = ctx.r4.u32 & 0xFFu;
    if (!is_main_target) {
      uint32_t width = 0;
      uint32_t height = 0;
      SurfaceSize(target, width, height);
      if (ctx.r5.u32 == 0 && width != 0 && height != 0 && width <= 2048 && height <= 2048) {
        flags |= 0x40000000u | ((width - 1) << 8) | ((height - 1) << 19);
      } else {
        flags &= ~0x10u;
      }
    }
    graphics->NoteGuestClearColor(flags, color, whole_target,
                                  static_cast<float>(ctx.f1.f64));
  } else if (graphics) {
    graphics->NoteGuestClear(ctx.r4.u32, static_cast<float>(ctx.f1.f64), ctx.r8.u32);
  }
  __imp__D3DDevice_ClearF(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  Trace(fmt::format("DrawIndexed prim={} rt={:08X}", ctx.r4.u32, CurrentRenderTarget()));
  // The declaration read where it is used, so the stride that goes with it is
  // known. One sample says little; several with different strides pin down how
  // the words are framed, because the stride bounds every offset in them.
  {
    static std::mutex mutex;
    static std::set<uint32_t> strides_seen;
    uint32_t decl = 0;
    uint32_t stride = 0;
    {
      std::lock_guard lock(g_state_mutex);
      decl = g_state.vertex_declaration;
      stride = g_state.streams[0].stride;
    }
    bool fresh = false;
    if (stride != 0 && decl >= 0x10000000u && decl < 0x80000000u) {
      std::lock_guard lock(mutex);
      fresh = strides_seen.size() < 6 && strides_seen.insert(stride).second;
    }
    if (fresh) {
      std::string words;
      for (uint32_t i = 8; i < 24; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(decl + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: declaration {:08X} stride {}: w8..w23:{}", decl, stride, words);
    }
  }
  // r3 device, r4 primitive type, r5 base vertex, r6 start index, r7 primitives
  DescribeDraw("DrawIndexedVertices", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  uint32_t target = 0;
  {
    std::lock_guard lock(g_state_mutex);
    target = g_state.render_target;
  }
  const bool into_frame = DrawingIntoFrame(target);
  CountFrameRouting(into_frame);
  auto* graphics = into_frame ? ActiveGraphicsSystem() : nullptr;
  if (graphics) {
    // The title hands the device to every draw, so this is the one place it
    // can be learned without guessing. Without it the constants are never read
    // out of the device at all, and the scene draws with no transform.
    graphics->BindGuestD3DDevice(ctx.r3.u32);
    rex::system::IGraphicsSystem::GuestDrawBuffers buffers;
    {
      std::lock_guard lock(g_state_mutex);
      buffers.vertex_buffer =
          BufferDataAddress(g_state.streams[0].buffer) + g_state.streams[0].offset;
      buffers.vertex_stride = g_state.streams[0].stride;
      buffers.index_buffer = BufferDataAddress(g_state.index_buffer);
      if (g_state.index_buffer >= 0x10000000u) {
        buffers.index_32bit = (GuestWord(g_state.index_buffer) & 0x80000000u) != 0;
      }
      for (uint32_t s = 0; s < 4; ++s) {
        if (g_state.streams[s].buffer != 0) {
          buffers.stream_address[s] =
              BufferDataAddress(g_state.streams[s].buffer) + g_state.streams[s].offset;
          buffers.stream_stride[s] = g_state.streams[s].stride;
        }
      }
    }
    buffers.base_vertex = ctx.r5.u32;
    buffers.start_index = ctx.r6.u32;
    {
      std::lock_guard lock(g_state_mutex);
      buffers.vertex_shader_object = g_state.vertex_shader;
      buffers.pixel_shader_object = g_state.pixel_shader;
      for (uint32_t slot = 0; slot < 8; ++slot) {
        buffers.textures[slot] = g_state.textures[slot];
      }
      // Slot zero decodes to a one-by-one texture while slot one decodes
      // correctly, though both are read the same way. Seeing the objects
      // themselves says whether that is what the title bound or whether the
      // descriptor is somewhere else in this build.
      {
        static std::atomic<uint32_t> shown{0};
        if (ctx.r7.u32 >= 1000 && shown.fetch_add(1) < 3) {
          for (uint32_t slot = 0; slot < 3; ++slot) {
            const uint32_t texture = g_state.textures[slot];
            std::string words;
            if (texture >= 0x10000000u) {
              // Read it both ways. The window the GPU reads through is offset
              // from the plain physical address, and which of the two applies
              // here has to be seen rather than assumed.
              for (uint32_t i = 0; i < 10; ++i) {
                const auto* w = GuestPtr<const rex::be_u32>(texture + i * 4);
                words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
              }
              if ((texture & 0xF0000000u) == 0xE0000000u) {
                // A physical address, so read as one: taken as a virtual
                // address it lands where nothing is mapped on Windows, and
                // the title faulted here on its first 3D frame.
                const uint32_t windowed = (texture & 0x1FFFFFFFu) + 0x1000u;
                auto* memory = REX_KERNEL_MEMORY();
                words += " |";
                for (uint32_t i = 0; i < 10; ++i) {
                  const auto* w =
                      memory ? memory->TranslatePhysical<const rex::be_u32*>(windowed + i * 4)
                             : nullptr;
                  words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
                }
              }
            }
            REXLOG_INFO("plume: texture slot {} object {:08X}:{}", slot, texture, words);
          }
        }
      }
    }
    // DrawIndexedVertices(device, primitiveType, baseVertexIndex, startIndex,
    // indexCount) - the last argument is already a count of indices, and
    // converting it from primitives, as this did, inflated every draw.
    // Record the draw after the title's own call has run: that call is where
    // Direct3D loads the shaders for it, and the load is what identifies them.
    const uint32_t primitive_type = ctx.r4.u32;
    const uint32_t index_count = ctx.r7.u32;
    __imp__D3DDevice_DrawIndexedVertices(ctx, base);
    graphics->NoteGuestDraw(primitive_type, index_count, buffers);
    return;
  }
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}


// The title builds its shader objects itself - it never calls
// CreateVertexShader - so the only place to learn what one stands for is where
// it is bound. Once per object: the association does not change.
namespace {
void RegisterShaderObject(uint32_t object, bool pixel_shader) {
  if (object < 0x10000000u || object >= 0x80000000u) {
    return;
  }
  static std::mutex mutex;
  static std::set<uint32_t> seen;
  {
    std::lock_guard lock(mutex);
    if (!seen.insert(object).second) {
      return;
    }
  }
  auto* graphics = ActiveGraphicsSystem();
  const auto* host = GuestPtr<const uint8_t>(object);
  if (graphics && host) {
    graphics->RegisterGuestShaderObject(object, host, pixel_shader);
  }
}
}  // namespace

REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  NoteD3D(0, ctx);
  // The title never calls CreateVertexShader, so the association between its
  // shader objects and the microcode has to come from the object itself. Dump
  // the first words of a few of them: one of them points at the microcode the
  // IM_LOAD packets carry, and that is what identifies the translated shader.
  {
    // SetVertexShader is also called with small non-pointer values, and
    // GuestPtr does not check: reading one faults the process.
    static std::atomic<uint32_t> dumped{0};
    const uint32_t object = ctx.r4.u32;
    if (object >= 0x10000000u && object < 0x80000000u && dumped.fetch_add(1) < 3) {
      std::string words;
      for (uint32_t i = 0; i < 12; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(object + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: vertex shader object {:08X}:{}", object, words);
    }
  }
  {
    std::lock_guard lock(g_state_mutex);
    g_state.device = ctx.r3.u32;
    g_state.vertex_shader = ctx.r4.u32;
  }
  RegisterShaderObject(ctx.r4.u32, false);
  __imp__D3DDevice_SetVertexShader(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  NoteD3D(1, ctx);
  // Vertex shader objects keep a pointer to their microcode at +0x28; pixel
  // ones hold zero there, so their pointer sits elsewhere. Finding it is what
  // lets a shader be named by what it is rather than by the order it happened
  // to be bound in - which is what has been resolving these to the wrong
  // shader and unpacking scene vertices with the interface's layout.
  {
    static std::atomic<uint32_t> dumped{0};
    const uint32_t object = ctx.r4.u32;
    if (object >= 0x10000000u && object < 0x80000000u && dumped.fetch_add(1) < 4) {
      std::string words;
      for (uint32_t i = 0; i < 16; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(object + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: pixel shader object {:08X}:{}", object, words);
    }
  }
  {
    std::lock_guard lock(g_state_mutex);
    g_state.pixel_shader = ctx.r4.u32;
  }
  RegisterShaderObject(ctx.r4.u32, true);
  __imp__D3DDevice_SetPixelShader(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetVertexDeclaration) {
  NoteD3D(2, ctx);
  // Where each field sits inside a vertex. Taking the offsets from the
  // shader's own fetch instructions instead - which is what the direct path
  // does now - only works when the two happen to agree, and the garbage
  // geometry says they do not. Dump the object once to find its layout.
  {
    static std::atomic<uint32_t> dumped{0};
    const uint32_t decl = ctx.r4.u32;
    if (decl >= 0x10000000u && decl < 0x80000000u && dumped.fetch_add(1) < 8) {
      std::string words;
      for (uint32_t i = 0; i < 28; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(decl + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: vertex declaration {:08X}:{}", decl, words);
      // The same declaration beside the stride of the stream it describes:
      // the framing of these words is not obvious on its own, and the stride
      // bounds where the last field can end, which is what makes it legible.
      std::lock_guard lock(g_state_mutex);
      REXLOG_INFO("plume:   stream 0 stride {} buffer {:08X}", g_state.streams[0].stride,
                  g_state.streams[0].buffer);
    }
  }
  {
    std::lock_guard lock(g_state_mutex);
    g_state.vertex_declaration = ctx.r4.u32;
  }
  __imp__D3DDevice_SetVertexDeclaration(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetStreamSource) {
  NoteD3D(3, ctx);
  {
    // device, stream index, buffer, byte offset, stride
    std::lock_guard lock(g_state_mutex);
    const uint32_t stream = ctx.r4.u32;
    if (stream < 4) {
      g_state.streams[stream] = D3DStream{ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    }
  }
  __imp__D3DDevice_SetStreamSource(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetIndices) {
  NoteD3D(4, ctx);
  // The index buffer object should say how wide its indices are. Reading them
  // as sixteen bits when they are thirty-two - or the other way about - gives
  // geometry assembled from the wrong vertices, which is what the direct path
  // is producing.
  {
    static std::atomic<uint32_t> dumped{0};
    const uint32_t ib = ctx.r4.u32;
    if (ib >= 0x10000000u && ib < 0x80000000u && dumped.fetch_add(1) < 4) {
      std::string words;
      for (uint32_t i = 0; i < 12; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(ib + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: index buffer object {:08X}:{}", ib, words);
    }
  }
  {
    std::lock_guard lock(g_state_mutex);
    g_state.index_buffer = ctx.r4.u32;
  }
  __imp__D3DDevice_SetIndices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetTexture) {
  NoteD3D(5, ctx);
  {
    std::lock_guard lock(g_state_mutex);
    const uint32_t sampler = ctx.r4.u32;
    if (sampler < 16) {
      g_state.textures[sampler] = ctx.r5.u32;
    }
  }
  __imp__D3DDevice_SetTexture(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  Trace(fmt::format("SetRenderTarget slot={} {:08X}", ctx.r4.u32, ctx.r5.u32));
  NoteD3D(6, ctx);
  // Every distinct surface the title renders into, once, with its size. The
  // scene is drawn into several - the frame itself and smaller ones for
  // shadows, reflections and glow - and drawing all of them straight into the
  // window is what covers the screen in blue planes.
  {
    static std::mutex seen_mutex;
    static std::set<uint32_t> seen;
    const uint32_t surface = ctx.r5.u32;
    bool fresh = false;
    {
      std::lock_guard lock(seen_mutex);
      fresh = surface != 0 && seen.size() < 32 && seen.insert(surface).second;
    }
    if (fresh) {
      const auto* w = GuestPtr<const rex::be_u32>(surface + 0x18);
      const uint32_t size = w ? static_cast<uint32_t>(*w) : 0u;
      REXLOG_INFO("plume: render target surface {:08X} slot {} is {}x{} (word {:08X}) "
                  "+10={:08X} +14={:08X} +1C={:08X} +20={:08X} +24={:08X}",
                  surface, ctx.r4.u32, (size & 0x1FFFu) + 1, ((size >> 13) & 0x1FFFu) + 1, size,
                  GuestWord(surface + 0x10), GuestWord(surface + 0x14), GuestWord(surface + 0x1C),
                  GuestWord(surface + 0x20), GuestWord(surface + 0x24));
    }
  }
  // The title's Direct3D decides whether the frame needs tiling from what the
  // render target is - its size, format and how much of the on-chip memory it
  // would take. This hook lives in the title, so it reports the same thing
  // under either backend, and the two can be compared directly. That is the
  // one place the decision can still differ, now that the command streams
  // agree everywhere else.
  {
    static std::atomic<uint32_t> logged{0};
    const uint32_t surface = ctx.r5.u32;
    if (logged.fetch_add(1) < 6 && surface >= 0x10000000u && surface < 0x80000000u) {
      std::string words;
      for (uint32_t i = 0; i < 10; ++i) {
        const auto* w = GuestPtr<const rex::be_u32>(surface + i * 4);
        words += fmt::format(" {:08X}", w ? static_cast<uint32_t>(*w) : 0u);
      }
      REXLOG_INFO("plume: SetRenderTarget slot={} surface={:08X}:{}", ctx.r4.u32, surface, words);
    }
  }
  {
    std::lock_guard lock(g_state_mutex);
    // Slot zero is the one the frame is drawn into. The title binds slot one
    // as well, and unbinds it with a null surface - taking whichever slot was
    // set last made the current target "nothing" after every such unbind,
    // and every draw after it was routed away from the screen.
    if (ctx.r4.u32 == 0) {
      g_state.render_target = ctx.r5.u32;
    }
  }
  __imp__D3DDevice_SetRenderTarget(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawVertices) {
  Trace(fmt::format("DrawVertices prim={} rt={:08X}", ctx.r4.u32, CurrentRenderTarget()));
  NoteD3D(7, ctx);
  DescribeDraw("DrawVertices", ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32);
  // DrawVertices(device, primitiveType, startVertex, vertexCount)
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t start_vertex = ctx.r5.u32;
  const uint32_t vertex_count = ctx.r6.u32;
  const bool into_frame = DrawingIntoFrame(CurrentRenderTarget());
  rex::system::IGraphicsSystem::GuestDrawBuffers buffers;
  {
    std::lock_guard lock(g_state_mutex);
    buffers.vertex_buffer =
        BufferDataAddress(g_state.streams[0].buffer) + g_state.streams[0].offset;
    buffers.vertex_stride = g_state.streams[0].stride;
    // Every stream, as for indexed draws: the props' hand-instanced shaders
    // read a second one, and stream 0 alone put its fetches in the wrong
    // buffer.
    for (uint32_t s = 0; s < 4; ++s) {
      if (g_state.streams[s].buffer != 0) {
        buffers.stream_address[s] =
            BufferDataAddress(g_state.streams[s].buffer) + g_state.streams[s].offset;
        buffers.stream_stride[s] = g_state.streams[s].stride;
      }
    }
  }
  buffers.base_vertex = start_vertex;
  TakeDrawState(buffers);
  // After the title's own call, which is where its shaders get loaded.
  __imp__D3DDevice_DrawVertices(ctx, base);
  // Under the same switch as the interface: with the ring drawing, these reach
  // the screen through it already.
  if (!into_frame || !InterfaceFromDirect3D()) {
    return;
  }
  if (auto* graphics = ActiveGraphicsSystem()) {
    graphics->NoteGuestDraw(primitive_type, vertex_count, buffers);
  }
}

// D3DDevice_BeginVertices(device, primitive type, vertex count, stride) hands
// back memory for the title to write its vertices into - and, per the beta's
// code, has already written the whole draw by then: a fetch constant pointing
// at that memory and a DRAW_INDX with the count and primitive type. There is
// no EndVertices to wait for. The shaders are loaded inside it as well
// (ProcessLazyState), so they are identified by the time it returns.
//
// The vertices themselves are written after it returns, and are read when the
// frame is encoded, by which time they are there.
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  Trace(fmt::format("BeginVertices prim={} n={} rt={:08X}", ctx.r4.u32, ctx.r5.u32, CurrentRenderTarget()));
  NoteD3D(8, ctx);
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;
  const uint32_t target = CurrentRenderTarget();
  __imp__D3DDevice_BeginVertices(ctx, base);
  const uint32_t vertices = ctx.r3.u32;
  if (!InterfaceFromDirect3D() || !vertices || !vertex_count || !stride ||
      !DrawingIntoFrame(target)) {
    return;
  }
  auto* graphics = ActiveGraphicsSystem();
  if (!graphics) {
    return;
  }
  {
    static std::atomic<uint32_t> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
      REXLOG_INFO("plume: BeginVertices prim={} count={} stride={} -> vertices at {:08X}",
                  primitive_type, vertex_count, stride, vertices);
    }
  }
  rex::system::IGraphicsSystem::GuestDrawBuffers buffers;
  buffers.vertex_buffer = vertices;
  buffers.vertex_stride = stride;
  TakeDrawState(buffers);
  graphics->NoteGuestDraw(primitive_type, vertex_count, buffers);
}

REX_HOOK_RAW(D3DDevice_BeginTiling) {
  Trace("BeginTiling");
  NoteD3D(9, ctx);
  // The reference backend sees about twenty-four tiling packets for every one
  // of these calls and this one sees none, though the call itself happens just
  // as often. So the decision is made inside, from these arguments - most
  // likely the tile count - and they are worth seeing before reading three
  // hundred lines of its body.
  {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 4) {
      REXLOG_INFO("plume: BeginTiling r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
                  "r8={:08X} r9={:08X}",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                  ctx.r9.u32);
    }
  }
  __imp__D3DDevice_BeginTiling(ctx, base);
}

REX_HOOK_RAW(D3DDevice_EndTiling) {
  Trace(fmt::format("EndTiling dest={:08X}", ctx.r6.u32));
  NoteD3D(10, ctx);
  // EndTiling(device, flags, resolve rects, destination texture, clear colour,
  // ...) resolves the tiled frame into its destination itself, tile by tile -
  // the scene's copy into the front buffer happens here, not through
  // D3DDevice_Resolve. Without this marker the last copy of a 3D frame was
  // some other target's, and that is what got shown: black.
  if (RenderTargetsFromDirect3D() && ctx.r6.u32 >= 0x10000000u) {
    if (auto* graphics = ActiveGraphicsSystem()) {
      uint32_t width = 0;
      uint32_t height = 0;
      SurfaceSize(CurrentRenderTarget(), width, height);
      graphics->NoteGuestResolve(ctx.r4.u32 | 0x80000000u,
                                 GpuPhysical(GuestWord(ctx.r6.u32 + 0x14) & 0xFFFFF000u), width,
                                 height);
    }
  }
  __imp__D3DDevice_EndTiling(ctx, base);
}

REX_HOOK_RAW(D3DDevice_CreateTexture) {
  NoteD3D(11, ctx);
  __imp__D3DDevice_CreateTexture(ctx, base);
}

REX_HOOK_RAW(D3DDevice_CreateVertexBuffer) {
  NoteD3D(12, ctx);
  __imp__D3DDevice_CreateVertexBuffer(ctx, base);
}

REX_HOOK_RAW(D3DDevice_CreateIndexBuffer) {
  NoteD3D(13, ctx);
  __imp__D3DDevice_CreateIndexBuffer(ctx, base);
}

// CDevice::SetFence in the beta symbols. It builds the value BlockOnFence then
// waits for, out of the device's own lap counter and submission counter - the
// same pair the backend publishes into the identifier block. Logging what it
// lays down beside what we publish says whether the two agree.
REX_HOOK_RAW(sub_82382578) {
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 || (n % 20000) == 0) {
    const uint32_t device = ctx.r3.u32;
    const auto rd = [&](uint32_t at) -> uint32_t {
      const auto* p = GuestPtr<const rex::be_u32>(at);
      return p ? static_cast<uint32_t>(*p) : 0u;
    };
    const uint32_t block = rd(device + 10384);
    REXLOG_INFO(
        "plume: SetFence #{}: lap {:08X}, submitted {:08X}, block holds {:08X}/{:08X}", n,
        rd(device + 14020), rd(device + 10396), block ? rd(block) : 0u,
        block ? rd(block + 4) : 0u);
  }
  __imp__sub_82382578(ctx, base);
}
