#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PixelInterpolator {
    int usage_index = 0;
    int usage = 0;
    int register_index = 0;
};

bool ParsePixelInterpolator(const std::string& token, PixelInterpolator* out);

void WrapRuntimePixelShader(const std::vector<uint8_t>& microcode, const std::string& output_path,
                            const std::vector<PixelInterpolator>& interpolators, uint32_t outputs);
