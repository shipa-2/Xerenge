#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>

struct XdvdfsImage {
    std::ifstream file;                        // the image opened by path
    std::unique_ptr<std::streambuf> fd_buffer;  // or read from a descriptor
    std::istream in{nullptr};                   // whichever of the two
    uint64_t base = 0;

    explicit XdvdfsImage(const std::string& path);
#ifndef _WIN32
    // An open descriptor, read where it stands rather than reopened by path:
    // on Android a file from the system picker is reachable only that way.
    explicit XdvdfsImage(int fd);
#endif
    std::vector<uint8_t> Sector(uint32_t number, uint32_t count = 1);
    void Root(uint32_t* sector, uint32_t* size);

private:
    void FindVolume();
};

using XdvdfsReportFn = std::function<void(const std::string& path, uint64_t length)>;

void XdvdfsExtract(XdvdfsImage& image, uint32_t sector, uint32_t size, const std::string& destination,
                   bool listing_only, const XdvdfsReportFn& report);
