#include "file_util.hpp"
#include "shader_containers.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    float min_similarity = 0.6f;
    std::string image_path;
    std::string micro_path;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--min-similarity" && i + 1 < argc) {
            min_similarity = std::stof(argv[++i]);
        } else if (arg[0] == '-') {
            std::cerr << "usage: rehost-runtime-shader image microcode out.container "
                         "[--min-similarity F]\n";
            return 1;
        } else if (image_path.empty()) {
            image_path = arg;
        } else if (micro_path.empty()) {
            micro_path = arg;
        } else if (output_path.empty()) {
            output_path = arg;
        } else {
            std::cerr << "usage: rehost-runtime-shader image microcode out.container\n";
            return 1;
        }
    }
    if (image_path.empty() || micro_path.empty() || output_path.empty()) {
        std::cerr << "usage: rehost-runtime-shader image microcode out.container\n";
        return 1;
    }

    try {
        const auto image_bytes = ReadFileBytes(image_path);
        const std::string image(reinterpret_cast<const char*>(image_bytes.data()), image_bytes.size());
        const auto code_bytes = ReadFileBytes(micro_path);
        if (code_bytes.empty() || code_bytes.size() % kInstructionSize != 0) {
            std::cerr << micro_path << ": microcode must be whole 12-byte instructions\n";
            return 1;
        }
        const std::string code(reinterpret_cast<const char*>(code_bytes.data()), code_bytes.size());

        float best_score = -1.0f;
        const ContainerMatch* best = nullptr;
        std::string best_template;
        for (const ContainerMatch& match : FindContainers(image)) {
            if (match.physical_size != code.size()) {
                continue;
            }
            const std::string templ =
                image.substr(match.offset + match.virtual_size, match.physical_size);
            const float score = MicrocodeSimilarity(templ, code);
            if (best == nullptr || score > best_score) {
                best_score = score;
                best = &match;
                best_template = templ;
            }
        }
        if (best == nullptr) {
            std::cerr << micro_path << ": no container of " << code.size()
                      << " bytes in the image\n";
            return 1;
        }
        if (best_score < min_similarity) {
            std::cerr << micro_path << ": best template at 0x" << std::hex << best->offset
                      << std::dec << " shares only " << int(best_score * 100)
                      << "% of instructions - too little to call it the same shader\n";
            return 1;
        }

        std::vector<uint8_t> container(
            image_bytes.begin() + static_cast<ptrdiff_t>(best->offset),
            image_bytes.begin() + static_cast<ptrdiff_t>(best->offset + best->virtual_size +
                                                          best->physical_size));
        std::memcpy(container.data() + best->virtual_size, code.data(), code.size());
        WriteFileBytes(output_path, container);

        const int differing =
            static_cast<int>(std::lround((1.0f - best_score) * (code.size() / kInstructionSize)));
        std::cout << micro_path << ": template at 0x" << std::hex << best->offset << std::dec
                  << ", " << int(best_score * 100) << "% of instructions identical (" << differing
                  << " patched) -> " << output_path << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
