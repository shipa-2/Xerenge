/**
 * @file        plume_renderer/plume_dxc_link.cpp
 * @brief       D3D12: a XenosRecomp shader with specialisation constants is a
 *              DXIL library calling g_SpecConstants(); linked here against one
 *              that returns the pipeline's value, as re:Blue does
 *              (gpu/shaders/dxc_link.cpp). dxcompiler.dll is loaded when first
 *              needed, so builds and machines without it are untouched.
 */
#include "plume_renderer/plume_dxc_link.h"

#if defined(_WIN32)

#include <windows.h>
#include <unknwn.h>

#include <dxcapi.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace rex::plume_renderer {
namespace {

template <typename T>
class Com {
 public:
  Com() = default;
  ~Com() {
    if (p_) {
      p_->Release();
    }
  }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  T** put() { return &p_; }
  T* operator->() const { return p_; }
  T* get() const { return p_; }

 private:
  T* p_ = nullptr;
};

DxcCreateInstanceProc CreateInstance() {
  static const DxcCreateInstanceProc proc = [] {
    HMODULE module = LoadLibraryW(L"dxcompiler.dll");
    return module ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"))
                  : nullptr;
  }();
  return proc;
}

std::vector<uint8_t> ToVector(IDxcBlob* blob) {
  const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
  return std::vector<uint8_t>(bytes, bytes + blob->GetBufferSize());
}

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value) {
  DxcCreateInstanceProc create = CreateInstance();
  Com<IDxcCompiler3> compiler;
  if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())))) {
    return {};
  }
  char hlsl[128];
  const int length = std::snprintf(hlsl, sizeof(hlsl), "export uint g_SpecConstants() { return %u; }", value);
  DxcBuffer buffer{};
  buffer.Ptr = hlsl;
  buffer.Size = SIZE_T(length);
  buffer.Encoding = DXC_CP_ACP;
  const wchar_t* args[] = {L"-T", L"lib_6_3"};
  Com<IDxcResult> result;
  if (FAILED(compiler->Compile(&buffer, args, 2, nullptr, IID_PPV_ARGS(result.put()))) || !result.get()) {
    return {};
  }
  Com<IDxcBlob> blob;
  if (FAILED(result->GetResult(blob.put())) || !blob.get() || blob->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(blob.get());
}

std::mutex g_mutex;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_spec_libs;

const std::vector<uint8_t>* SpecConstantLib(uint32_t value) {
  std::lock_guard lock(g_mutex);
  auto it = g_spec_libs.find(value);
  if (it == g_spec_libs.end()) {
    it = g_spec_libs.emplace(value, CompileSpecConstantLib(value)).first;
  }
  return it->second.empty() ? nullptr : &it->second;
}

}  // namespace

std::vector<uint8_t> LinkSpecConstants(const uint8_t* library, size_t library_size, bool pixel_shader,
                                       uint32_t spec_constants) {
  const std::vector<uint8_t>* spec = SpecConstantLib(spec_constants);
  DxcCreateInstanceProc create = CreateInstance();
  if (!spec || !create || !library || library_size == 0) {
    return {};
  }
  Com<IDxcUtils> utils;
  if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())))) {
    return {};
  }
  Com<IDxcBlobEncoding> spec_blob;
  Com<IDxcBlobEncoding> shader_blob;
  if (FAILED(utils->CreateBlobFromPinned(spec->data(), UINT32(spec->size()), DXC_CP_ACP, spec_blob.put())) ||
      FAILED(utils->CreateBlobFromPinned(library, UINT32(library_size), DXC_CP_ACP, shader_blob.put()))) {
    return {};
  }
  Com<IDxcLinker> linker;
  if (FAILED(create(CLSID_DxcLinker, IID_PPV_ARGS(linker.put())))) {
    return {};
  }
  linker->RegisterLibrary(L"SpecConstants", spec_blob.get());
  linker->RegisterLibrary(L"Shader", shader_blob.get());
  const wchar_t* libraries[] = {L"SpecConstants", L"Shader"};
  Com<IDxcOperationResult> result;
  if (FAILED(linker->Link(L"shaderMain", pixel_shader ? L"ps_6_0" : L"vs_6_0", libraries, 2, nullptr, 0,
                          result.put())) ||
      !result.get()) {
    return {};
  }
  // A failed link shows in the operation's status, not the call's result.
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
    Com<IDxcBlobEncoding> errors;
    if (SUCCEEDED(result->GetErrorBuffer(errors.put())) && errors.get() && errors->GetBufferSize() != 0) {
      std::fprintf(stderr, "DXC link: %.*s\n", int(errors->GetBufferSize()),
                   static_cast<const char*>(errors->GetBufferPointer()));
    }
    return {};
  }
  Com<IDxcBlob> linked;
  if (FAILED(result->GetResult(linked.put())) || !linked.get() || linked->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(linked.get());
}

}  // namespace rex::plume_renderer

#else

namespace rex::plume_renderer {

std::vector<uint8_t> LinkSpecConstants(const uint8_t*, size_t, bool, uint32_t) {
  return {};
}

}  // namespace rex::plume_renderer

#endif
