#include "runtime_shader_recipe.hpp"

#include "file_util.hpp"
#include "shader_containers.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr float kMinSimilarity = 0.6f;

struct SourceFile {
    std::string name;
    std::string data;
};

uint32_t BeU32(const std::string& data, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, data.data() + offset, 4);
    return __builtin_bswap32(v);
}

std::vector<SourceFile> CollectSources(const std::string& image_path, const std::string& game_path) {
    std::vector<SourceFile> out;
    const auto image_bytes = ReadFileBytes(image_path);
    out.push_back({"image",
                   std::string(reinterpret_cast<const char*>(image_bytes.data()), image_bytes.size())});

    const std::filesystem::path graphics = std::filesystem::path(game_path) / "graphics";
    if (std::filesystem::is_directory(graphics)) {
        std::vector<std::filesystem::path> objs;
        for (const auto& entry : std::filesystem::directory_iterator(graphics)) {
            if (entry.path().extension() == ".obj") {
                objs.push_back(entry.path());
            }
        }
        std::sort(objs.begin(), objs.end());
        for (const auto& path : objs) {
            const auto bytes = ReadFileBytes(path.string());
            const std::string rel =
                std::filesystem::relative(path, std::filesystem::path(game_path)).string();
            out.push_back({rel, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
        }
    }
    return out;
}

struct TemplateHit {
    float score = 0.0f;
    std::string source;
    std::string micro;
};

const TemplateHit* BestTemplate(std::string_view code, const std::vector<SourceFile>& files) {
    TemplateHit best;
    const TemplateHit* found = nullptr;
    for (const SourceFile& file : files) {
        for (const ContainerMatch& match : FindContainers(file.data)) {
            if (match.physical_size != code.size()) {
                continue;
            }
            const size_t off = match.offset + match.virtual_size;
            const std::string_view micro(file.data.data() + off, match.physical_size);
            const float score = MicrocodeSimilarity(micro, code);
            if (found == nullptr || score > best.score) {
                best.score = score;
                best.source = file.name;
                best.micro.assign(micro.begin(), micro.end());
                found = &best;
            }
        }
    }
    if (found == nullptr || found->score < kMinSimilarity) {
        return nullptr;
    }
    return found;
}

std::vector<std::string> Patches(const std::string& template_micro, const std::string& code) {
    std::vector<std::string> out;
    const size_t count = code.size() / kInstructionSize;
    for (size_t i = 0; i < count; ++i) {
        for (int word = 0; word < 3; ++word) {
            const size_t at = i * kInstructionSize + static_cast<size_t>(word) * 4;
            const uint32_t a = BeU32(template_micro, at);
            const uint32_t b = BeU32(code, at);
            const uint32_t mask = a ^ b;
            if (mask == 0) {
                continue;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%zu:%d:%08x:%08x", i, word, mask, b & mask);
            out.emplace_back(buf);
        }
    }
    return out;
}

const SourceFile* FindBareMicrocode(std::string_view code, const std::vector<SourceFile>& files) {
    for (const SourceFile& file : files) {
        if (file.data.find(code) != std::string::npos) {
            return &file;
        }
    }
    return nullptr;
}

std::string FirstWrapDecl(const std::filesystem::path& wrapargs) {
    if (!std::filesystem::is_regular_file(wrapargs)) {
        return {};
    }
    std::ifstream in(wrapargs);
    std::string line;
    while (std::getline(in, line)) {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        return line.substr(start);
    }
    return {};
}

}  // namespace

int MakeRuntimeShaderRecipe(const std::string& image_path, const std::string& game_path,
                            const std::string& dump_dir, const std::string& output_path) {
    try {
        const std::vector<SourceFile> files = CollectSources(image_path, game_path);
        std::vector<std::string> lines = {
            "# Runtime-assembled shaders, rebuilt at install time by",
            "# build_runtime_shaders from the user's own disc. Written by",
            "# make_runtime_shader_recipe; holds hashes and patch masks only.",
            "#",
            "# <stage> <name> <sha256 of result> template <file> <sha256 of template> [patches]",
            "# <stage> <name> <sha256 of result> raw <file> <declarations for the wrapper>",
        };

        std::vector<std::filesystem::path> micros;
        for (const auto& entry : std::filesystem::directory_iterator(dump_dir)) {
            if (entry.path().extension() == ".bin") {
                micros.push_back(entry.path());
            }
        }
        std::sort(micros.begin(), micros.end());

        int missing = 0;
        for (const auto& micro_path : micros) {
            const std::string name = micro_path.stem().string();
            const std::string stage = name.substr(0, name.find('_'));
            const auto code_bytes = ReadFileBytes(micro_path.string());
            const std::string code(reinterpret_cast<const char*>(code_bytes.data()), code_bytes.size());

            if (const TemplateHit* hit = BestTemplate(code, files)) {
                const std::string code_hash = Sha256Hex(code);
                const std::string template_hash = Sha256Hex(hit->micro);
                std::string row = stage + " " + name + " " + code_hash + " template " + hit->source +
                                  " " + template_hash;
                for (const std::string& patch : Patches(hit->micro, code)) {
                    row += " ";
                    row += patch;
                }
                lines.push_back(std::move(row));
                continue;
            }

            const SourceFile* source = FindBareMicrocode(code, files);
            const std::string decl = FirstWrapDecl(micro_path.parent_path() / (name + ".wrapargs"));
            if (source != nullptr && !decl.empty()) {
                lines.push_back(stage + " " + name + " " + Sha256Hex(code) + " raw " + source->name +
                               " " + decl);
                continue;
            }

            std::cerr << name << ": no source found on the disc\n";
            ++missing;
        }

        std::string text;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) {
                text += '\n';
            }
            text += lines[i];
        }
        text += '\n';
        WriteFileText(output_path, text);

        std::cerr << "wrote " << output_path << ": " << (lines.size() - 6)
                  << " shaders, " << missing << " without a source\n";
        return missing ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
