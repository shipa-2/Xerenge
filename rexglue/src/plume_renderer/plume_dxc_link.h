/**
 * @file        plume_renderer/plume_dxc_link.h
 * @brief       D3D12: links a XenosRecomp DXIL library with the value its
 *              g_SpecConstants() returns (plume_dxc_link.cpp).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::plume_renderer {

// The vertex or pixel shader (vs_6_0 / ps_6_0) the library makes with these
// specialisation constants; empty on failure or without dxcompiler.dll.
std::vector<uint8_t> LinkSpecConstants(const uint8_t* library, size_t library_size, bool pixel_shader,
                                       uint32_t spec_constants);

}  // namespace rex::plume_renderer
