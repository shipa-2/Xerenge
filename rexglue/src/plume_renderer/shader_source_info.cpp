#include "plume_renderer/shader_source_info.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace rex::plume_renderer {

namespace {

// The vertex constant registers a shader can name.
constexpr uint32_t kRegisters = 256;

// The registers holding a shader's placement matrices: each named constant
// runs from its start to the next one's.
std::vector<uint16_t> ParseMatrixRegisters(const std::string& hlsl) {
  std::vector<uint16_t> out;
  static constexpr const char* kMatrices[] = {
      "gObjToProjMatrix",       "gCurrentWorldViewMatrix", "gPrevWorldViewMatrix",
      "gObjToWorldMatrix",      "gWorldToProjMatrix",      "gObjToWorldRotationMatrix",
      "gVertexShader_BlendMatrices", "gTransformArray"};
  // Every named vertex constant and where it starts.
  std::vector<std::pair<uint32_t, bool>> starts;  // register, is a matrix
  static constexpr std::string_view kDefine = "#define g";
  static constexpr std::string_view kBase = "g_PushConstants.VertexShaderConstants + ";
  for (size_t at = hlsl.find(kDefine); at != std::string::npos; at = hlsl.find(kDefine, at + 1)) {
    const size_t eol = hlsl.find('\n', at);
    const std::string line = hlsl.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    const size_t base = line.find(kBase);
    if (base == std::string::npos) {
      continue;
    }
    // "+ (START + min(INDEX, ...)) * 16" for arrays, "+ OFFSET," for single ones.
    size_t num = base + kBase.size();
    uint32_t reg = 0;
    if (line[num] == '(') {
      reg = uint32_t(std::strtoul(line.c_str() + num + 1, nullptr, 10));
    } else {
      reg = uint32_t(std::strtoul(line.c_str() + num, nullptr, 10)) / 16;
    }
    const std::string id = line.substr(8, line.find_first_of("( ", 8) - 8);
    bool matrix = false;
    for (const char* m : kMatrices) {
      matrix |= id == m;
    }
    starts.emplace_back(reg, matrix);
  }
  std::sort(starts.begin(), starts.end());
  for (size_t i = 0; i < starts.size(); ++i) {
    if (!starts[i].second) {
      continue;
    }
    const uint32_t end = i + 1 < starts.size() ? starts[i + 1].first : kRegisters;
    for (uint32_t r = starts[i].first; r < end && r < kRegisters; ++r) {
      out.push_back(uint16_t(r));
    }
  }
  return out;
}

// The input locations a vertex shader reads, in the order its body first
// reads them - the order of its vertex fetch instructions. Declaration order
// is not it: the declarations follow the container's element array.
std::vector<uint32_t> ParseConsumedLocations(std::string_view src) {
  std::unordered_map<std::string_view, uint32_t> location_of;
  for (size_t at = src.find("[[vk::location("); at != std::string_view::npos;
       at = src.find("[[vk::location(", at + 1)) {
    const size_t open = at + std::string_view("[[vk::location(").size();
    const size_t close = src.find(')', open);
    if (close == std::string_view::npos) {
      break;
    }
    uint32_t value = 0;
    bool digits = close > open;
    for (size_t i = open; i < close && digits; ++i) {
      digits = src[i] >= '0' && src[i] <= '9';
      value = value * 10 + uint32_t(src[i] - '0');
    }
    const size_t name_at = src.find(" i", close);
    const size_t name_end = src.find_first_of(" :;[", name_at + 1);
    if (!digits || name_at == std::string_view::npos || name_end == std::string_view::npos) {
      continue;
    }
    location_of.emplace(src.substr(name_at + 1, name_end - name_at - 1), value);
  }

  std::vector<uint32_t> consumed;
  for (size_t at = src.find("input.i"); at != std::string_view::npos;
       at = src.find("input.i", at + 1)) {
    const size_t name_at = at + std::string_view("input.").size();
    const size_t name_end = src.find_first_of(")., ;[", name_at);
    if (name_end == std::string_view::npos) {
      break;
    }
    const std::string_view input_name = src.substr(name_at, name_end - name_at);
    // The guest index XenosRecomp hands instancing shaders is not a vertex
    // fetch; counted as one it shifted every real input a place along.
    if (input_name == "iGuestIndex") {
      continue;
    }
    const auto it = location_of.find(input_name);
    if (it != location_of.end() &&
        std::find(consumed.begin(), consumed.end(), it->second) == consumed.end()) {
      consumed.push_back(it->second);
    }
  }
  return consumed;
}

}  // namespace

ShaderSourceInfo ParseShaderSource(std::string_view src) {
  ShaderSourceInfo info;
  info.guest_index = src.find("iGuestIndex") != std::string_view::npos;
  if (const size_t body = src.find("shaderMain(");
      body != std::string_view::npos && src.find("output.oPos", body) != std::string_view::npos &&
      src.find("tfetch2D(", body) != std::string_view::npos) {
    info.vertex_fetch = true;
  }
  for (size_t at = src.find("output.oPos.xy"); at != std::string_view::npos;
       at = src.find("output.oPos.xy", at + 1)) {
    const size_t eol = src.find('\n', at);
    const std::string_view line = src.substr(at, eol == std::string_view::npos ? eol : eol - at);
    if (line.find("HalfPixel") == std::string_view::npos &&
        (line.find('*') != std::string_view::npos || line.find(" + ") != std::string_view::npos)) {
      info.position_scaling = true;
      break;
    }
  }
  info.writes_oc1 = src.find("output.oC1") != std::string_view::npos;
  static constexpr std::string_view kSamplerKey =
      "_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + ";
  for (size_t at = src.find(kSamplerKey); at != std::string_view::npos;
       at = src.find(kSamplerKey, at + 1)) {
    const std::string digits(src.substr(at + kSamplerKey.size(), 12));
    const uint32_t offset = uint32_t(std::strtoul(digits.c_str(), nullptr, 10));
    if (offset < 64) {
      info.sampler_slots |= 1u << (offset / 4);
    }
  }
  info.consumed_locations = ParseConsumedLocations(src);
  info.matrix_registers = ParseMatrixRegisters(std::string(src));
  return info;
}

const ShaderSourceInfo* FindShaderSourceInfo(uint64_t hash) {
  static const std::unordered_map<uint64_t, ShaderSourceInfo> table = [] {
    std::unordered_map<uint64_t, ShaderSourceInfo> filled;
    FillShaderSourceInfoTable(filled);
    return filled;
  }();
  if (const auto it = table.find(hash); it != table.end()) {
    return &it->second;
  }
  // Not in the table: a shader translated after the build, in a development
  // tree that still has its source.
  static std::mutex mutex;
  static std::unordered_map<uint64_t, std::unique_ptr<ShaderSourceInfo>> read;
  std::lock_guard lock(mutex);
  if (const auto it = read.find(hash); it != read.end()) {
    return it->second.get();
  }
  std::unique_ptr<ShaderSourceInfo> info;
  char name[64];
  std::snprintf(name, sizeof(name), "generated/xenos-hlsl/%016llx.hlsl",
                static_cast<unsigned long long>(hash));
  if (std::ifstream file{name, std::ios::binary}) {
    const std::string hlsl((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (!hlsl.empty()) {
      info = std::make_unique<ShaderSourceInfo>(ParseShaderSource(hlsl));
    }
  }
  return (read[hash] = std::move(info)).get();
}

}  // namespace rex::plume_renderer
