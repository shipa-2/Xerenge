#include "file_util.hpp"
#include "wrap_runtime_pixel_shader.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: wrap-runtime-pixel-shader microcode out.container "
                     "[--interpolator U:usage:R] [--outputs MASK]\n";
        return 1;
    }
    std::vector<PixelInterpolator> interpolators;
    uint32_t outputs = 0x1;
    std::string micro_path = argv[1];
    std::string output_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--interpolator" && i + 1 < argc) {
            PixelInterpolator ip{};
            if (!ParsePixelInterpolator(argv[++i], &ip)) {
                std::cerr << "bad --interpolator\n";
                return 1;
            }
            interpolators.push_back(ip);
        } else if (arg == "--outputs" && i + 1 < argc) {
            outputs = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }
    try {
        WrapRuntimePixelShader(ReadFileBytes(micro_path), output_path, interpolators, outputs);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
