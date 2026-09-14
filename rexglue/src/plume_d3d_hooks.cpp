/**
 * @file        plume_d3d_hooks.cpp
 * @brief       Guest D3D hooks for the plume backend (phase 3).
 */
#include "plume_d3d.h"

#include <atomic>
#include <cstring>

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
  PatchGuestFunction(dispatcher, kBeginVerticesGuestAddr, &BeginVerticesRuntime,
                     "D3DDevice_BeginVertices");
  PatchGuestFunction(dispatcher, kEndVerticesGuestAddr, &EndVerticesRuntime,
                     "D3DDevice_EndVertices");
}

extern "C" void __imp__Direct3D_CreateDevice(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_Reset(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_Clear(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_Swap(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_DrawVerticesUP(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__D3DDevice_DrawIndexedVertices(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8246E100(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8246E158(PPCContext& __restrict, uint8_t*);

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

REX_HOOK_RAW(D3DDevice_Reset) {
  // 0x8246B538 is a mid-function fragment; skipping it skips the stack restore.
  __imp__D3DDevice_Reset(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Clear) {
  __imp__D3DDevice_Clear(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Swap) {
  if (PlumePresentationActive()) {
    // r3 is not the device here (this guest address is a post-bctrl epilogue).
    PlumePresentFromDevice(0);
  }
  __imp__D3DDevice_Swap(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawVerticesUP) {
  // 0x8248FBC8 is a stack-restore epilogue, not DrawVerticesUP.
  __imp__D3DDevice_DrawVerticesUP(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  // 0x82490030 is a shared epilogue (mr r3,r29; addi r1,128; restgprlr).
  // Stubbing it (log 159) skipped the restore and the next store hit guest 0x0.
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}
