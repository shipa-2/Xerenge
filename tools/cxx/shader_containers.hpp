#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

constexpr uint32_t kContainerVersions[] = {0x102A1100, 0x102A0E00};
constexpr uint32_t kContainerHeaderSize = 36;
constexpr uint32_t kInstructionSize = 12;

struct ContainerMatch {
    size_t offset = 0;
    uint32_t virtual_size = 0;
    uint32_t physical_size = 0;
};

std::vector<ContainerMatch> FindContainers(std::string_view image);
float MicrocodeSimilarity(std::string_view a, std::string_view b);
