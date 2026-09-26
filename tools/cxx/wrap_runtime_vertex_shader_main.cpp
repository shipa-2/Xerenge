#include "file_util.hpp"
#include "wrap_runtime_vertex_shader.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: wrap-runtime-vertex-shader microcode out.container "
                     "--element A:usage:I [--interpolator U:usage:R] [--sampler N]\n";
        return 1;
    }
    std::vector<VertexElement> elements;
    std::vector<VertexInterpolator> interpolators;
    std::vector<int> samplers;
    std::string micro_path = argv[1];
    std::string output_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--element" && i + 1 < argc) {
            VertexElement el{};
            if (!ParseVertexElement(argv[++i], &el)) {
                std::cerr << "bad --element\n";
                return 1;
            }
            elements.push_back(el);
        } else if (arg == "--interpolator" && i + 1 < argc) {
            VertexInterpolator ip{};
            if (!ParseVertexInterpolator(argv[++i], &ip)) {
                std::cerr << "bad --interpolator\n";
                return 1;
            }
            interpolators.push_back(ip);
        } else if (arg == "--sampler" && i + 1 < argc) {
            samplers.push_back(std::stoi(argv[++i], nullptr, 0));
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }
    if (elements.empty()) {
        std::cerr << "at least one --element is required\n";
        return 1;
    }
    try {
        WrapRuntimeVertexShader(ReadFileBytes(micro_path), output_path, elements, interpolators,
                                samplers);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
