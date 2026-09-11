#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Serves the guest's D:\ file namespace either from an Xbox 360 disc image
// (XDVDFS inside an .iso) or from a plain host directory holding the same
// files. The directory path is preferred: it drops the XDVDFS layer, avoids
// re-reading the whole image, and is what `--extract-game` produces.
class XboxMedia
{
public:
    // Accepts either an .iso disc image or a directory. Directories are
    // detected automatically.
    bool open(const std::string& path);
    bool isOpen() const;

    // Walk the mounted XDVDFS image and write every file to <outDir>,
    // preserving the (lower-cased) directory layout. Only valid after open()
    // on an .iso.
    bool extractTo(const std::string& outDir) const;

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
        // Set in directory mode; when present all I/O goes through this
        // stream instead of the shared image and sector arithmetic.
        std::shared_ptr<std::ifstream> hostFile;
        uint64_t hostSize = 0;
    };

    bool readAt(uint64_t offset, void* destination, size_t size) const;
    bool parseDirectory(uint32_t startSector, uint32_t size, const std::string& prefix);
    bool parseNode(uint64_t tableOffset, uint32_t tableSize, uint64_t nodeOffset,
        const std::string& prefix, bool rootNode);
    static std::string normalize(std::string path);

    mutable std::mutex mutex_;
    mutable std::ifstream image_;
    // A "trimmed" XDVDFS dump starts the volume descriptor at kHeaderOffset
    // from byte 0 of the file. A full raw disc dump (security sector + video
    // partition ahead of the game partition) puts the game partition itself
    // some way into the file first; open() probes a few known XGD layouts
    // for the volume magic and records the winning base here so every
    // subsequent sector offset is computed relative to the game partition,
    // not the file.
    uint64_t imageBaseOffset_ = 0;
    bool directoryMode_ = false;
    std::string root_;
    std::unordered_map<std::string, FileEntry> entries_;
    std::unordered_map<uint32_t, OpenFile> openFiles_;
    uint32_t nextHandle_ = 0xA0000000u;
};
