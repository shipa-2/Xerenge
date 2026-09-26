#include "file_util.hpp"
#include "shader_containers.hpp"
#include "sha256.hpp"
#include "wrap_runtime_pixel_shader.hpp"
#include "wrap_runtime_vertex_shader.hpp"

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct TemplateEntry {
    size_t offset = 0;
    uint32_t virtual_size = 0;
    uint32_t physical_size = 0;
};

class Sources {
 public:
    Sources(std::string image_path, std::string game_path)
        : image_path_(std::move(image_path)), game_path_(std::move(game_path)) {}

    std::string_view Read(const std::string& name) {
        auto it = data_.find(name);
        if (it != data_.end()) {
            return it->second;
        }
        const std::string path =
            name == "image" ? image_path_ : (std::filesystem::path(game_path_) / name).string();
        const auto bytes = ReadFileBytes(path);
        std::string stored(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        it = data_.emplace(name, std::move(stored)).first;
        return it->second;
    }

    const TemplateEntry* Template(const std::string& name, const std::string& digest) {
        auto& map = templates_[name];
        if (map.empty()) {
            const std::string data = std::string(Read(name));
            for (const ContainerMatch& match : FindContainers(data)) {
                const size_t micro_off = match.offset + match.virtual_size;
                const std::string_view micro(data.data() + micro_off, match.physical_size);
                const std::string hash = Sha256Hex(micro);
                if (!map.count(hash)) {
                    map.emplace(hash, TemplateEntry{match.offset, match.virtual_size,
                                                    match.physical_size});
                }
            }
        }
        const auto it = map.find(digest);
        return it == map.end() ? nullptr : &it->second;
    }

    std::vector<uint8_t> Raw(const std::string& name, const std::string& digest, size_t size) {
        const std::string data = std::string(Read(name));
        for (size_t offset = 0; offset + size <= data.size(); offset += 4) {
            const std::string_view chunk(data.data() + offset, size);
            if (Sha256Hex(chunk) == digest) {
                return std::vector<uint8_t>(chunk.begin(), chunk.end());
            }
        }
        return {};
    }

 private:
    std::string image_path_;
    std::string game_path_;
    std::unordered_map<std::string, std::string> data_;
    std::unordered_map<std::string, std::unordered_map<std::string, TemplateEntry>> templates_;
};

uint32_t BeU32(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, data.data() + offset, 4);
    return __builtin_bswap32(v);
}

void PutBeU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    const uint32_t be = __builtin_bswap32(value);
    std::memcpy(data.data() + offset, &be, 4);
}

bool WrapFromArgs(const std::string& stage, const std::vector<uint8_t>& microcode,
                  const std::string& output, const std::vector<std::string>& args) {
    if (stage == "PS") {
        std::vector<PixelInterpolator> interpolators;
        uint32_t outputs = 0x1;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--interpolator" && i + 1 < args.size()) {
                PixelInterpolator ip{};
                if (!ParsePixelInterpolator(args[++i], &ip)) {
                    return false;
                }
                interpolators.push_back(ip);
            } else if (args[i] == "--outputs" && i + 1 < args.size()) {
                outputs = static_cast<uint32_t>(std::stoul(args[++i], nullptr, 0));
            }
        }
        WrapRuntimePixelShader(microcode, output, interpolators, outputs);
        return true;
    }
    if (stage == "VS") {
        std::vector<VertexElement> elements;
        std::vector<VertexInterpolator> interpolators;
        std::vector<int> samplers;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--element" && i + 1 < args.size()) {
                VertexElement el{};
                if (!ParseVertexElement(args[++i], &el)) {
                    return false;
                }
                elements.push_back(el);
            } else if (args[i] == "--interpolator" && i + 1 < args.size()) {
                VertexInterpolator ip{};
                if (!ParseVertexInterpolator(args[++i], &ip)) {
                    return false;
                }
                interpolators.push_back(ip);
            } else if (args[i] == "--sampler" && i + 1 < args.size()) {
                samplers.push_back(std::stoi(args[++i], nullptr, 0));
            }
        }
        if (elements.empty()) {
            return false;
        }
        WrapRuntimeVertexShader(microcode, output, elements, interpolators, samplers);
        return true;
    }
    return false;
}

std::vector<std::string> SplitFields(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string token;
    while (stream >> token) {
        fields.push_back(token);
    }
    return fields;
}

size_t RawSizeFromName(const std::string& name) {
    const size_t pos = name.rfind('_');
    if (pos == std::string::npos) {
        return 0;
    }
    return static_cast<size_t>(std::stoul(name.substr(pos + 1), nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: build-runtime-shaders recipe guest-image.bin game-dir out-dir\n";
        return 1;
    }
    const std::string recipe_path = argv[1];
    const std::string image_path = argv[2];
    const std::string game_path = argv[3];
    const std::string output_dir = argv[4];

    try {
        std::filesystem::create_directories(output_dir);
        Sources sources(image_path, game_path);
        const std::string recipe = ReadFileText(recipe_path);
        int built = 0;
        int failed = 0;

        std::istringstream lines(recipe);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto fields = SplitFields(line);
            if (fields.size() < 5) {
                continue;
            }
            const std::string& stage = fields[0];
            const std::string& name = fields[1];
            const std::string& digest = fields[2];
            const std::string& kind = fields[3];
            const std::string& source = fields[4];
            const std::string target = (std::filesystem::path(output_dir) / (name + ".container")).string();

            try {
                if (kind == "template") {
                    if (fields.size() < 6) {
                        throw std::runtime_error("template line too short");
                    }
                    const std::string& template_digest = fields[5];
                    const TemplateEntry* entry = sources.Template(source, template_digest);
                    if (!entry) {
                        throw std::runtime_error("no template in " + source);
                    }
                    const std::string blob = std::string(sources.Read(source));
                    std::vector<uint8_t> container(
                        blob.begin() + static_cast<ptrdiff_t>(entry->offset),
                        blob.begin() + static_cast<ptrdiff_t>(entry->offset + entry->virtual_size +
                                                              entry->physical_size));
                    for (size_t i = 6; i < fields.size(); ++i) {
                        const std::string& field = fields[i];
                        const size_t c1 = field.find(':');
                        const size_t c2 = field.find(':', c1 + 1);
                        const size_t c3 = field.find(':', c2 + 1);
                        if (c1 == std::string::npos || c2 == std::string::npos ||
                            c3 == std::string::npos) {
                            throw std::runtime_error("bad patch field");
                        }
                        const int index = std::stoi(field.substr(0, c1), nullptr, 10);
                        const int word = std::stoi(field.substr(c1 + 1, c2 - c1 - 1), nullptr, 10);
                        const uint32_t mask = static_cast<uint32_t>(
                            std::stoul(field.substr(c2 + 1, c3 - c2 - 1), nullptr, 16));
                        const uint32_t value =
                            static_cast<uint32_t>(std::stoul(field.substr(c3 + 1), nullptr, 16));
                        const size_t at = entry->virtual_size +
                                          static_cast<size_t>(index) * kInstructionSize +
                                          static_cast<size_t>(word) * 4;
                        const uint32_t old = BeU32(container, at);
                        PutBeU32(container, at, (old & ~mask) | value);
                    }
                    const std::string_view micro(
                        reinterpret_cast<const char*>(container.data()) + entry->virtual_size,
                        entry->physical_size);
                    if (Sha256Hex(micro) != digest) {
                        throw std::runtime_error("patched result does not match");
                    }
                    WriteFileBytes(target, container);
                } else if (kind == "raw") {
                    const size_t size = RawSizeFromName(name);
                    const auto code = sources.Raw(source, digest, size);
                    if (code.empty()) {
                        throw std::runtime_error("not found in " + source);
                    }
                    const std::string micro_path =
                        (std::filesystem::path(output_dir) / (name + ".bin")).string();
                    WriteFileBytes(micro_path, code);
                    const std::vector<std::string> args(fields.begin() + 5, fields.end());
                    if (!WrapFromArgs(stage, code, target, args)) {
                        throw std::runtime_error("wrapper failed");
                    }
                    std::filesystem::remove(micro_path);
                } else {
                    throw std::runtime_error("unknown kind " + kind);
                }
                ++built;
            } catch (const std::exception& error) {
                std::cerr << name << ": " << error.what() << "\n";
                ++failed;
            }
        }

        std::cerr << "runtime shaders: " << built << " rebuilt from the disc, " << failed
                  << " failed\n";
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
