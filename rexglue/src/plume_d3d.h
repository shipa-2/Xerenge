/**
 * @file        plume_d3d.h
 * @brief       Guest D3D constants/offsets for plume D3D hooks (phase 3).
 */
#pragma once

#include <cstdint>

namespace xerenge::plume_d3d {

constexpr uint32_t kD3DDeviceAllocSize = 0x5000;

constexpr uint32_t kRenderStateTableGuestAddr = 0x82751D68;
constexpr uint32_t kRenderStateTableEntries = 0x61;
constexpr uint32_t kRenderStateDispatchOffset = 56;
constexpr uint32_t kRenderStateDefaultOffset = 524;

constexpr uint32_t kSamplerStateTableGuestAddr = 0x827521F8;
constexpr uint32_t kSamplerStateTableEntries = 0x14;
constexpr uint32_t kSamplerStateDispatchOffset = 444;
constexpr uint32_t kSamplerStateDefaultOffset = 912;

constexpr uint32_t kBlendControlNoBlend = 0x00010001;

constexpr uint32_t kDeviceRingOffset = 0x28;
constexpr uint32_t kDeviceFetchConstantsOffset = 0x400;
constexpr uint32_t kDeviceVsFloatConstantsOffset = 0x700;
constexpr uint32_t kDevicePsFloatConstantsOffset = 0x1700;
constexpr uint32_t kDeviceVsBoolConstantsOffset = 0x2700;
constexpr uint32_t kDevicePsBoolConstantsOffset = 0x2710;
constexpr uint32_t kDeviceViewportOffset = 0x3058;
constexpr uint32_t kDevicePresentParamsOffset = 0x3370;
constexpr uint32_t kDeviceResolutionAppliedOffset = 0x2A39;
constexpr uint32_t kDeviceResolutionWidthOffset = 0x4DFC;
constexpr uint32_t kDeviceRbBlendControl0Offset = 0x28B8;
constexpr uint32_t kDeviceRbBlendControl1Offset = 0x28D8;
constexpr uint32_t kDevicePixelShaderOffset = 0x3080;
constexpr uint32_t kDeviceVertexShaderOffset = 0x3084;

constexpr uint32_t kGuestShaderObjectSize = 24;

// Guest addresses that codegen cannot split (shared epilogues / absorbed into
// larger functions). Patched at runtime via FunctionDispatcher::SetFunction.
constexpr uint32_t kBeginVerticesGuestAddr = 0x8248F6F0;
constexpr uint32_t kEndVerticesGuestAddr = 0x8248FBA8;
constexpr uint32_t kRingBufferAllocGuestAddr = 0x8246DE10;
constexpr uint32_t kBeginRingAllocGuestAddr = 0x8246E018;
constexpr uint32_t kRingBufferSubmitBatchGuestAddr = 0x8246E100;
constexpr uint32_t kRingBufferWaitForSpaceGuestAddr = 0x8246E618;
constexpr uint32_t kBlockOnFenceGuestAddr = 0x8246E800;
constexpr uint32_t kRingBufferFlushGuestAddr = 0x8246EC50;
constexpr uint32_t kInsertFenceGuestAddr = 0x8246ED38;
constexpr uint32_t kBlockUntilIdleGuestAddr = 0x8246EEB8;
constexpr uint32_t kSetRingBufferParametersGuestAddr = 0x8246EF88;
constexpr uint32_t kRingBufferPollReadyGuestAddr = 0x8246B408;

void InstallPlumeRuntimeHooks();

}  // namespace xerenge::plume_d3d
