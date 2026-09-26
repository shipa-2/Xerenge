#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

struct XdvdfsImage {
    std::ifstream file;
    uint64_t base = 0;

    explicit XdvdfsImage(const std::string& path);
    std::vector<uint8_t> Sector(uint32_t number, uint32_t count = 1);
    void Root(uint32_t* sector, uint32_t* size);
};

using XdvdfsReportFn = std::function<void(const std::string& path, uint64_t length)>;

void XdvdfsExtract(XdvdfsImage& image, uint32_t sector, uint32_t size, const std::string& destination,
                   bool listing_only, const XdvdfsReportFn& report);
