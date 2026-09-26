#include "shader_containers.hpp"

#include <cstring>

namespace {

uint32_t BeU32(std::string_view data, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, data.data() + offset, 4);
    return __builtin_bswap32(v);
}

bool VersionMatch(uint32_t flags) {
    const uint32_t masked = flags & 0xFFFFFF00u;
    for (uint32_t version : kContainerVersions) {
        if (masked == version) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<ContainerMatch> FindContainers(std::string_view image) {
    std::vector<ContainerMatch> out;
    size_t i = 0;
    const size_t limit = image.size() > kContainerHeaderSize ? image.size() - kContainerHeaderSize : 0;
    while (i < limit) {
        const uint32_t flags = BeU32(image, i);
        if (VersionMatch(flags)) {
            const uint32_t virtual_size = BeU32(image, i + 4);
            const uint32_t physical_size = BeU32(image, i + 8);
            const uint64_t total = static_cast<uint64_t>(virtual_size) + physical_size;
            if (total > 0 && i + total <= image.size() && physical_size % kInstructionSize == 0) {
                out.push_back({i, virtual_size, physical_size});
                i += static_cast<size_t>(total);
                continue;
            }
        }
        i += 4;
    }
    return out;
}

float MicrocodeSimilarity(std::string_view a, std::string_view b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }
    const size_t count = a.size() / kInstructionSize;
    size_t same = 0;
    for (size_t i = 0; i < count; ++i) {
        if (a.substr(i * kInstructionSize, kInstructionSize) ==
            b.substr(i * kInstructionSize, kInstructionSize)) {
            ++same;
        }
    }
    return static_cast<float>(same) / static_cast<float>(count);
}
