#include "file_util.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* kSymbols[] = {
    "g_shaderCacheEntries",
    "g_shaderCacheEntryCount",
    "g_shaderMicrocodeEntries",
    "g_shaderMicrocodeEntryCount",
    "g_compressedSpirvCache",
    "g_spirvCacheCompressedSize",
    "g_spirvCacheDecompressedSize",
};

std::string Transform(const std::string& src, const std::string& suffix) {
    std::string out = src;
    std::vector<const char*> names(std::begin(kSymbols), std::end(kSymbols));
    std::sort(names.begin(), names.end(),
              [](const char* a, const char* b) { return std::strlen(a) > std::strlen(b); });
    for (const char* name : names) {
        const std::string from = name;
        const std::string to = from + suffix;
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    for (const char* base :
         {"g_shaderCacheEntryCount", "g_shaderMicrocodeEntryCount", "g_spirvCacheCompressedSize",
          "g_spirvCacheDecompressedSize"}) {
        const std::string name = std::string(base) + suffix;
        const std::string from = "const size_t " + name;
        const std::string to = "extern const size_t " + name;
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    {
        const std::string array = "const uint8_t g_compressedSpirvCache" + suffix + "[]";
        const std::string repl = "extern const uint8_t g_compressedSpirvCache" + suffix + "[]";
        size_t pos = 0;
        while ((pos = out.find(array, pos)) != std::string::npos) {
            out.replace(pos, array.size(), repl);
            pos += repl.size();
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "--suffix") {
        std::cerr << "usage: prepare-aux-shader-cache input.cpp output.cpp --suffix NAME\n";
        return 1;
    }
    const std::string suffix = argv[4];
    try {
        WriteFileText(argv[2], Transform(ReadFileText(argv[1]), suffix));
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
