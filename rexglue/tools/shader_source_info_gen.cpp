// Reads every translated shader in generated/xenos-hlsl and writes what the
// renderer needs to know about each (see shader_source_info.h) as a C++
// table, so the renderer carries those facts and not the shader sources.
//
//   shader_source_info_gen <generated/xenos-hlsl> <output.cpp>
//
// A missing directory gives an empty table: the renderer then reads the
// sources itself where they exist.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

#include "plume_renderer/shader_source_info.h"

namespace fs = std::filesystem;
using rex::plume_renderer::ParseShaderSource;
using rex::plume_renderer::ShaderSourceInfo;

// The generator links shader_source_info.cpp, which refers to the table it
// is about to write.
void rex::plume_renderer::FillShaderSourceInfoTable(
    std::unordered_map<uint64_t, ShaderSourceInfo>&) {}

namespace {

template <typename T>
std::string List(const std::vector<T>& values) {
  std::string out = "{";
  for (size_t i = 0; i < values.size(); ++i) {
    out += (i ? ", " : "") + std::to_string(values[i]);
  }
  return out + "}";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <generated/xenos-hlsl> <output.cpp>\n", argv[0]);
    return 2;
  }
  const fs::path dir = argv[1];
  std::map<uint64_t, ShaderSourceInfo> shaders;
  std::error_code error;
  if (fs::is_directory(dir, error)) {
    for (const auto& entry : fs::directory_iterator(dir)) {
      const fs::path& path = entry.path();
      const std::string stem = path.stem().string();
      if (path.extension() != ".hlsl" || stem.size() != 16) {
        continue;
      }
      char* end = nullptr;
      const uint64_t hash = std::strtoull(stem.c_str(), &end, 16);
      if (*end != '\0') {
        continue;
      }
      std::ifstream file(path, std::ios::binary);
      const std::string hlsl((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
      if (!hlsl.empty()) {
        shaders.emplace(hash, ParseShaderSource(hlsl));
      }
    }
  }

  std::ostringstream out;
  out << "// Written by tools/shader_source_info_gen from generated/xenos-hlsl.\n"
         "#include \"plume_renderer/shader_source_info.h\"\n\n"
         "namespace rex::plume_renderer {\n\n"
         "void FillShaderSourceInfoTable(std::unordered_map<uint64_t, ShaderSourceInfo>& t) {\n";
  for (const auto& [hash, info] : shaders) {
    char key[32];
    std::snprintf(key, sizeof(key), "0x%016llXull", static_cast<unsigned long long>(hash));
    out << "  t.emplace(" << key << ", ShaderSourceInfo{" << (info.guest_index ? "true" : "false")
        << ", " << (info.vertex_fetch ? "true" : "false") << ", "
        << (info.position_scaling ? "true" : "false") << ", "
        << (info.writes_oc1 ? "true" : "false") << ", " << info.sampler_slots << "u, "
        << List(info.consumed_locations) << ", " << List(info.matrix_registers) << "});\n";
  }
  out << "}\n\n}  // namespace rex::plume_renderer\n";

  // Rewritten only when it changes, so an unchanged table rebuilds nothing.
  const std::string text = out.str();
  {
    std::ifstream existing(argv[2], std::ios::binary);
    const std::string old((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
    if (old == text) {
      return 0;
    }
  }
  std::ofstream file(argv[2], std::ios::binary);
  file << text;
  if (!file) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  std::printf("shader_source_info_gen: %zu shader(s)\n", shaders.size());
  return 0;
}
