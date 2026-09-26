#include "wrap_runtime_vertex_shader.hpp"

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

bool ParseVertexElement(const std::string& token, VertexElement* out) {
    const size_t c1 = token.find(':');
    const size_t c2 = token.rfind(':');
    if (c1 == std::string::npos || c2 == std::string::npos || c1 == c2) {
        return false;
    }
    try {
        out->address = std::stoi(token.substr(0, c1), nullptr, 0);
        out->usage = UsageFromName(token.substr(c1 + 1, c2 - c1 - 1));
        out->usage_index = std::stoi(token.substr(c2 + 1), nullptr, 0);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseVertexInterpolator(const std::string& token, VertexInterpolator* out) {
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

void WrapRuntimeVertexShader(const std::vector<uint8_t>& microcode, const std::string& output_path,
                             const std::vector<VertexElement>& elements,
                             const std::vector<VertexInterpolator>& interpolators,
                             const std::vector<int>& samplers_in) {
    if (microcode.empty() || microcode.size() % 4 != 0) {
        throw std::runtime_error("microcode must contain big-endian 32-bit words");
    }

    std::vector<int> samplers = samplers_in;
    std::sort(samplers.begin(), samplers.end());
    samplers.erase(std::unique(samplers.begin(), samplers.end()), samplers.end());
    for (int reg : samplers) {
        if (reg < 0 || reg > 0xFFFF) {
            throw std::runtime_error("sampler register does not fit the declaration");
        }
    }

    constexpr size_t kConstantTableOffset = 0x28;
    constexpr size_t kConstantInfoSize = 20;
    constexpr size_t kConstantTableHeaderSize = 28;

    const size_t constant_count = 1 + samplers.size();
    const size_t info_offset = kConstantTableOffset + kConstantTableHeaderSize;
    const size_t names_offset = info_offset + kConstantInfoSize * constant_count;

    std::vector<uint8_t> names = {'c', 0};
    std::unordered_map<std::string, size_t> name_offsets = {{"c", 0}};
    for (int reg : samplers) {
        const std::string key = "s" + std::to_string(reg);
        name_offsets[key] = names.size();
        for (char c : key) {
            names.push_back(static_cast<uint8_t>(c));
        }
        names.push_back(0);
    }

    const size_t shader_offset = (names_offset + names.size() + 3) & ~size_t(3);
    const size_t array_offset = shader_offset + 0x24;
    const size_t virtual_size = array_offset + 4 * (elements.size() + interpolators.size());
    std::vector<uint8_t> data(virtual_size + microcode.size(), 0);

    const uint32_t header[] = {0x102A1101, static_cast<uint32_t>(virtual_size),
                               static_cast<uint32_t>(microcode.size()), 0,
                               static_cast<uint32_t>(kConstantTableOffset - 4), 0,
                               static_cast<uint32_t>(shader_offset), 0, 0};
    for (size_t i = 0; i < 9; ++i) {
        PutWord(data, i * 4, header[i]);
    }

    PutWord(data, 0x24, static_cast<uint32_t>(kConstantTableHeaderSize +
                                              kConstantInfoSize * constant_count + names.size()));
    const uint32_t table[] = {static_cast<uint32_t>(kConstantTableHeaderSize), 0, 0,
                              static_cast<uint32_t>(constant_count),
                              static_cast<uint32_t>(kConstantTableHeaderSize), 0, 0};
    for (size_t i = 0; i < 7; ++i) {
        PutWord(data, kConstantTableOffset + i * 4, table[i]);
    }

    auto put_constant = [&](size_t slot, const std::string& name, uint16_t register_set,
                            uint16_t register_index, uint16_t register_count) {
        const size_t at = info_offset + slot * kConstantInfoSize;
        PutWord(data, at, static_cast<uint32_t>(names_offset - kConstantTableOffset + name_offsets.at(name)));
        PutHalf(data, at + 4, register_set);
        PutHalf(data, at + 6, register_index);
        PutHalf(data, at + 8, register_count);
    };

    put_constant(0, "c", 2, 0, 256);
    for (size_t slot = 0; slot < samplers.size(); ++slot) {
        put_constant(slot + 1, "s" + std::to_string(samplers[slot]), 3,
                     static_cast<uint16_t>(samplers[slot]), 1);
    }

    std::memcpy(data.data() + names_offset, names.data(), names.size());

    const uint32_t shader[] = {0, static_cast<uint32_t>(microcode.size()), 0, 0, 0,
                               static_cast<uint32_t>(interpolators.size()) << 5, 0,
                               static_cast<uint32_t>(elements.size()), 0};
    for (size_t i = 0; i < 9; ++i) {
        PutWord(data, shader_offset + i * 4, shader[i]);
    }

    for (size_t i = 0; i < elements.size(); ++i) {
        const auto& el = elements[i];
        PutWord(data, array_offset + i * 4,
                static_cast<uint32_t>(el.address | (el.usage << 12) | (el.usage_index << 16)));
    }
    const size_t interpolator_offset = array_offset + elements.size() * 4;
    for (size_t i = 0; i < interpolators.size(); ++i) {
        const auto& ip = interpolators[i];
        PutWord(data, interpolator_offset + i * 4,
                static_cast<uint32_t>(ip.usage_index | (ip.usage << 4) | (ip.register_index << 8)));
    }

    std::memcpy(data.data() + virtual_size, microcode.data(), microcode.size());

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + output_path);
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}
