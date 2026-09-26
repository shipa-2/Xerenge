#include "sha256.hpp"
#include "xdvdfs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void Usage() {
    std::cerr << "usage: extract-image [--list] [--sha256 HEX] image.iso [destination]\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool list_only = false;
    std::string sha256_expected;
    std::string image_path;
    std::string destination;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (std::strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) {
            sha256_expected = argv[++i];
        } else if (argv[i][0] == '-') {
            Usage();
            return 1;
        } else if (image_path.empty()) {
            image_path = argv[i];
        } else if (destination.empty()) {
            destination = argv[i];
        } else {
            Usage();
            return 1;
        }
    }

    if (image_path.empty() || (!list_only && destination.empty())) {
        Usage();
        return 1;
    }

    if (!sha256_expected.empty()) {
        std::string digest;
        if (!Sha256FileHex(image_path, &digest, nullptr)) {
            std::cerr << "cannot hash " << image_path << "\n";
            return 1;
        }
        std::transform(digest.begin(), digest.end(), digest.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string expected = sha256_expected;
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (digest != expected) {
            std::cerr << "checksum mismatch:\n  got      " << digest << "\n  expected "
                      << expected << "\n";
            return 1;
        }
        std::cerr << "checksum matches\n";
    }

    try {
        XdvdfsImage image(image_path);
        std::fprintf(stderr, "volume found at offset 0x%X\n", static_cast<unsigned>(image.base));
        uint32_t root_sector = 0;
        uint32_t root_size = 0;
        image.Root(&root_sector, &root_size);

        int files = 0;
        uint64_t total = 0;
        XdvdfsReportFn report = [&](const std::string& path, uint64_t length) {
            if (!path.empty() && path.back() != '/') {
                ++files;
                total += length;
            }
            if (list_only) {
                if (!path.empty() && path.back() == '/') {
                    std::cout << path << "\n";
                } else {
                    std::cout << path << "\t" << length << "\n";
                }
            } else if (files % 200 == 0) {
                std::fprintf(stderr, "\rfiles written: %d", files);
            }
        };

        XdvdfsExtract(image, root_sector, root_size, destination.empty() ? "." : destination,
                      list_only, report);
        if (!list_only) {
            std::fprintf(stderr, "\r");
        }
        std::fprintf(stderr, "%d files, %.1f GiB in total\n", files,
                     static_cast<double>(total) / (1 << 30));
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
