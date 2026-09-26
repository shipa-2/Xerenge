#include "xdvdfs.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

constexpr uint32_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint64_t kPartitionOffsets[] = {0x00000000, 0x0000FD90, 0x00002080, 0x0FD90000,
                                          0x18300000, 0x1FB20000};
constexpr uint8_t kAttributeDirectory = 0x10;

struct DirEntry {
    std::string name;
    uint32_t start = 0;
    uint32_t length = 0;
    bool is_directory = false;
};

void WalkTable(XdvdfsImage& image, uint32_t sector, uint32_t size, uint32_t offset,
               const std::function<void(const DirEntry&)>& emit) {
    if (size == 0) {
        return;
    }
    const auto table = image.Sector(sector, (size + kSector - 1) / kSector);
    std::vector<uint32_t> pending = {offset};
    std::set<uint32_t> seen;
    while (!pending.empty()) {
        const uint32_t at = pending.back() * 4;
        pending.pop_back();
        if (seen.count(at) || at + 14 > table.size()) {
            continue;
        }
        seen.insert(at);
        uint16_t left = 0;
        uint16_t right = 0;
        uint32_t start = 0;
        uint32_t length = 0;
        uint8_t attributes = 0;
        uint8_t name_length = 0;
        std::memcpy(&left, table.data() + at, 2);
        std::memcpy(&right, table.data() + at + 2, 2);
        std::memcpy(&start, table.data() + at + 4, 4);
        std::memcpy(&length, table.data() + at + 8, 4);
        attributes = table[at + 12];
        name_length = table[at + 13];
        if (left == 0xFFFF) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(table.data() + at + 14), name_length);
        if (!name.empty()) {
            emit(DirEntry{name, start, length, (attributes & kAttributeDirectory) != 0});
        }
        if (left) {
            pending.push_back(left);
        }
        if (right) {
            pending.push_back(right);
        }
    }
}

void CopyFile(XdvdfsImage& image, uint32_t start, uint64_t length, const std::string& destination) {
    std::ofstream out(destination, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + destination);
    }
    image.file.seekg(static_cast<std::streamoff>(image.base + static_cast<uint64_t>(start) * kSector));
    uint64_t remaining = length;
    std::vector<char> chunk(1 << 20);
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), remaining));
        image.file.read(chunk.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = image.file.gcount();
        if (got <= 0) {
            throw std::runtime_error("the image ends partway through " + destination);
        }
        out.write(chunk.data(), got);
        remaining -= static_cast<uint64_t>(got);
    }
}

}  // namespace

XdvdfsImage::XdvdfsImage(const std::string& path) {
    file.open(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    for (uint64_t candidate : kPartitionOffsets) {
        file.seekg(static_cast<std::streamoff>(candidate + 32 * kSector));
        char magic[sizeof(kMagic) - 1] = {};
        file.read(magic, sizeof(magic));
        if (file && std::memcmp(magic, kMagic, sizeof(magic)) == 0) {
            base = candidate;
            return;
        }
    }
    throw std::runtime_error("not an Xbox game disc image: no volume found");
}

std::vector<uint8_t> XdvdfsImage::Sector(uint32_t number, uint32_t count) {
    std::vector<uint8_t> out(static_cast<size_t>(count) * kSector);
    const auto pos = static_cast<std::streamoff>(base + static_cast<uint64_t>(number) * kSector);
    file.seekg(pos);
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(file.gcount()));
    return out;
}

void XdvdfsImage::Root(uint32_t* sector, uint32_t* size) {
    const auto descriptor = Sector(32, 1);
    if (descriptor.size() < 0x18) {
        throw std::runtime_error("volume descriptor too short");
    }
    std::memcpy(sector, descriptor.data() + 0x14, 4);
    std::memcpy(size, descriptor.data() + 0x18, 4);
}

void XdvdfsExtract(XdvdfsImage& image, uint32_t sector, uint32_t size, const std::string& destination,
                   bool listing_only, const XdvdfsReportFn& report) {
    if (!listing_only) {
        std::filesystem::create_directories(destination);
    }
    WalkTable(image, sector, size, 0, [&](const DirEntry& entry) {
        const std::string path = destination + "/" + entry.name;
        if (entry.is_directory) {
            report(path + "/", 0);
            XdvdfsExtract(image, entry.start, entry.length, path, listing_only, report);
        } else {
            report(path, entry.length);
            if (!listing_only) {
                CopyFile(image, entry.start, entry.length, path);
            }
        }
    });
}
