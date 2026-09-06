#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

class XboxMedia
{
public:
    bool open(const std::string& path);
    bool isOpen() const;
    bool openFile(const std::string& xboxPath, uint32_t& handle, uint64_t& size);
    bool readFile(uint32_t handle, void* destination, uint32_t size, uint32_t& bytesRead);
    bool seekFile(uint32_t handle, int64_t distance, uint32_t method, uint64_t& position);
    bool position(uint32_t handle, uint64_t& position) const;
    bool fileSize(uint32_t handle, uint64_t& size) const;
    void closeFile(uint32_t handle);

private:
    struct FileEntry
    {
        uint32_t startSector = 0;
        uint32_t size = 0;
        bool directory = false;
    };

    struct OpenFile
    {
        FileEntry entry;
        uint64_t position = 0;
    };

    bool readAt(uint64_t offset, void* destination, size_t size) const;
    bool parseDirectory(uint32_t startSector, uint32_t size, const std::string& prefix);
    bool parseNode(uint64_t tableOffset, uint32_t tableSize, uint64_t nodeOffset,
        const std::string& prefix, bool rootNode);
    static std::string normalize(std::string path);

    mutable std::mutex mutex_;
    mutable std::ifstream image_;
    std::unordered_map<std::string, FileEntry> entries_;
    std::unordered_map<uint32_t, OpenFile> openFiles_;
    uint32_t nextHandle_ = 0xA0000000u;
};
