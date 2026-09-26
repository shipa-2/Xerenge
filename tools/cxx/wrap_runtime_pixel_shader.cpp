#include "wrap_runtime_pixel_shader.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace {

int UsageFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::unordered_map<std::string, int> kUsages = {
        {"position", 0},  {"blendweight", 1}, {"blendindices", 2}, {"normal", 3},
        {"pointsize", 4}, {"texcoord", 5},    {"tangent", 6},      {"binormal", 7},
        {"color", 10},
    };
    const auto it = kUsages.find(name);
    if (it == kUsages.end()) {
        throw std::runtime_error("unknown usage " + name);
    }
    return it->second;
}

void PutWord(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    const uint32_t be = __builtin_bswap32(value);
    std::memcpy(data.data() + offset, &be, 4);
}

void PutHalf(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    const uint16_t be = __builtin_bswap16(value);
    std::memcpy(data.data() + offset, &be, 2);
}

}  // namespace

bool ParsePixelInterpolator(const std::string& token, PixelInterpolator* out) {
    const size_t c1 = token.find(':');
    const size_t c2 = token.rfind(':');
    if (c1 == std::string::npos || c2 == std::string::npos || c1 == c2) {
        return false;
    }
    try {
        out->usage_index = std::stoi(token.substr(0, c1), nullptr, 0);
        out->usage = UsageFromName(token.substr(c1 + 1, c2 - c1 - 1));
        out->register_index = std::stoi(token.substr(c2 + 1), nullptr, 0);
        return true;
    } catch (...) {
        return false;
    }
}

void WrapRuntimePixelShader(const std::vector<uint8_t>& microcode, const std::string& output_path,
                            const std::vector<PixelInterpolator>& interpolators, uint32_t outputs) {
    if (microcode.empty() || microcode.size() % 4 != 0) {
        throw std::runtime_error("microcode must contain big-endian 32-bit words");
    }
    if (interpolators.size() > 16) {
        throw std::runtime_error("at most 16 interpolators fit in interpolatorInfo");
    }

    constexpr size_t kShaderOffset = 0x74;
    const size_t array_offset = kShaderOffset + 0x24;
    const size_t virtual_size = array_offset + 4 * interpolators.size();
    std::vector<uint8_t> data(virtual_size + microcode.size(), 0);

    const uint32_t header[] = {0x102A1100, static_cast<uint32_t>(virtual_size),
                               static_cast<uint32_t>(microcode.size()), 0, 0x24, 0,
                               static_cast<uint32_t>(kShaderOffset), 0, 0};
    for (size_t i = 0; i < 9; ++i) {
        PutWord(data, i * 4, header[i]);
    }

    PutWord(data, 0x24, 56);
    const uint32_t table[] = {28, 0, 0, 2, 28, 0, 0};
    for (size_t i = 0; i < 7; ++i) {
        PutWord(data, 0x28 + i * 4, table[i]);
    }

    constexpr size_t float4_constant = 0x44;
    PutWord(data, float4_constant, 68);
    PutHalf(data, float4_constant + 4, 2);
    PutHalf(data, float4_constant + 6, 0);
    PutHalf(data, float4_constant + 8, 256);

    constexpr size_t sampler_constant = 0x58;
    PutWord(data, sampler_constant, 70);
    PutHalf(data, sampler_constant + 4, 3);
    PutHalf(data, sampler_constant + 6, 0);
    PutHalf(data, sampler_constant + 8, 1);
    data[0x6C] = 'c';
    data[0x6D] = 0;
    data[0x6E] = 's';
    data[0x6F] = '0';
    data[0x70] = 0;

    const uint32_t shader[] = {0, static_cast<uint32_t>(microcode.size()), 0, 0, 0,
                               static_cast<uint32_t>(interpolators.size()) << 5, 0, outputs, 0};
    for (size_t i = 0; i < 9; ++i) {
        PutWord(data, kShaderOffset + i * 4, shader[i]);
    }

    for (size_t i = 0; i < interpolators.size(); ++i) {
        const auto& ip = interpolators[i];
        PutWord(data, array_offset + i * 4,
                static_cast<uint32_t>(ip.usage_index | (ip.usage << 4) | (ip.register_index << 8)));
    }

    std::memcpy(data.data() + virtual_size, microcode.data(), microcode.size());

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + output_path);
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}
