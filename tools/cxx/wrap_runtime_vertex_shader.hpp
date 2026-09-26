#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VertexElement {
    int address = 0;
    int usage = 0;
    int usage_index = 0;
};

struct VertexInterpolator {
    int usage_index = 0;
    int usage = 0;
    int register_index = 0;
};

bool ParseVertexElement(const std::string& token, VertexElement* out);
bool ParseVertexInterpolator(const std::string& token, VertexInterpolator* out);

void WrapRuntimeVertexShader(const std::vector<uint8_t>& microcode, const std::string& output_path,
                             const std::vector<VertexElement>& elements,
                             const std::vector<VertexInterpolator>& interpolators,
                             const std::vector<int>& samplers);
