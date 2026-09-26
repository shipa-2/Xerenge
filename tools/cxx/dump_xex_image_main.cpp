#include "dump_xex.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string xex;
    std::string output = "generated/xenos-scan/guest-image.bin";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output = argv[++i];
        } else if (arg[0] == '-') {
            std::cerr << "usage: dump-xex-image [xex] [-o output]\n";
            return 1;
        } else if (xex.empty()) {
            xex = arg;
        } else {
            std::cerr << "usage: dump-xex-image [xex] [-o output]\n";
            return 1;
        }
    }
    if (xex.empty()) {
        std::cerr << "usage: dump-xex-image [xex] [-o output]\n";
        return 1;
    }
    try {
        DumpXexToPe(xex, output);
        std::cout << "wrote " << output << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
