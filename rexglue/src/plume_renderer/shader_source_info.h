#pragma once

// What the renderer needs to know about a translated shader that only its
// generated HLSL says: how its inputs are numbered, which constants hold its
// placement matrices, which sampler slots it reads, and a few traits.
//
// The HLSL is read at build time, not shipped: tools/shader_source_info_gen
// runs ParseShaderSource over generated/xenos-hlsl and writes the results
// into a table compiled into the renderer. A development build without that
// table falls back to reading generated/xenos-hlsl/<hash>.hlsl itself.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::plume_renderer {

struct ShaderSourceInfo {
  // XenosRecomp hands the shader the guest's vertex index: it instances by hand.
  bool guest_index = false;
  // A vertex shader that reads textures and writes a position.
  bool vertex_fetch = false;
  // Scales or offsets output.oPos.xy by something other than the half pixel.
  bool position_scaling = false;
  // Writes the second colour output.
  bool writes_oc1 = false;
  // Bit n: the shader reads the texture descriptor in shared-constant slot n.
  uint32_t sampler_slots = 0;
  // Its vertex input locations, in the order the body first reads them.
  std::vector<uint32_t> consumed_locations;
  // Vertex constant registers belonging to its placement matrices.
  std::vector<uint16_t> matrix_registers;
};

ShaderSourceInfo ParseShaderSource(std::string_view hlsl);

// The shader's facts, or null when neither the compiled-in table nor a
// generated/xenos-hlsl file knows it. The pointer stays valid.
const ShaderSourceInfo* FindShaderSourceInfo(uint64_t hash);

// Written by tools/shader_source_info_gen into the build directory.
void FillShaderSourceInfoTable(std::unordered_map<uint64_t, ShaderSourceInfo>& table);

}  // namespace rex::plume_renderer
